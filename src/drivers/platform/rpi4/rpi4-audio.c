/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's headphone jack (ws207-p002).
 *
 * PWM1 drives the jack's two channels on GPIO 40 and 41 (ALT0).  Its clock
 * is the 54 MHz oscillator halved by the clock manager, so a 48 kHz sample
 * is 563 PWM clocks; each sample is a duty word in the PWM's FIFO.  A legacy
 * DMA channel moves the duty words to the FIFO, paced by the PWM's DREQ,
 * through a cyclic chain of control blocks, one block a fragment of the
 * framework's ring, each raising an interrupt at its end.
 *
 * The framework's ring holds 16-bit samples the DMA cannot use, so the
 * driver keeps a second ring of duty words beside it.  At each interrupt it
 * converts the fragment the DMA has just entered and the one after it;
 * the next interrupt converts that one again with whatever was written
 * meanwhile, so the converted sound trails the samples by no more than the
 * interrupt latency.
 *
 * Playback only: the board has no microphone input.  No hardware volume:
 * audiod scales the samples.
 */

#include "drivers/platform/rpi4/rpi4-audio.h"
#include "drivers/platform/rpi4/rpi4-audio-ring.h"
#include "drivers/platform/rpi4/rpi4-board.h"
#include <drivers/audio/audio.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/audio.h>
#include <uapi/errno.h>

/* The most the firmware's tree is read. */
#define RPI4_AUDIO_FDT_LIMIT		(1024U * 1024U)

/* GPIO: the function select of pins 40 to 49, and ALT0. */
#define GPIO_GPFSEL4			0x10U
#define GPIO_FUNCTION_ALT0		4U
#define GPIO_JACK_FIRST			40U
#define GPIO_JACK_LAST			41U

/* PWM registers and bits (BCM2711 peripherals, 8.6). */
#define PWM_CTL				0x00U
#define PWM_STA				0x04U
#define PWM_DMAC			0x08U
#define PWM_RNG1			0x10U
#define PWM_FIF1			0x18U
#define PWM_RNG2			0x20U
#define PWM_REGISTERS_BYTES		0x28U
#define PWM_CTL_PWEN1			0x00000001U
#define PWM_CTL_USEF1			0x00000020U
#define PWM_CTL_CLRF			0x00000040U
#define PWM_CTL_PWEN2			0x00000100U
#define PWM_CTL_USEF2			0x00002000U
#define PWM_STA_ERRORS			0x000001fcU
#define PWM_DMAC_ENAB			0x80000000U
#define PWM_DMAC_THRESHOLDS		0x00000707U

/* PWM1 is the second block of the pair, the one wired to the jack. */
#define PWM1_OFFSET			0x800U

/* The clock manager's PWM clock (BCM2711 peripherals, 5.4). */
#define CM_PWMCTL			0xa0U
#define CM_PWMDIV			0xa4U
#define CM_REGISTERS_BYTES		0xa8U
#define CM_PASSWORD			0x5a000000U
#define CM_SOURCE_OSCILLATOR		1U
#define CM_ENABLE			0x00000010U
#define CM_BUSY				0x00000080U
#define CM_DIVI_SHIFT			12U

/*
 * The clock manager's divisor of the oscillator for the PWM.  Undivided
 * (1) the PWM never drains its FIFO on a Pi 4 although the clock reports
 * itself running, so the DMA stalls after the first words (BUG-318); 2,
 * 3 and 4 all play.  2 keeps the most duty steps (27 MHz).
 */
#define CM_PWM_DIVISOR			2U

/* One legacy DMA channel's registers (BCM2711 peripherals, 4.2.1). */
#define DMA_CHANNEL_STRIDE		0x100U
#define DMA_CHANNEL_LEGACY		7U
#define DMA_CS				0x00U
#define DMA_CONBLK_AD			0x04U
#define DMA_SOURCE_AD			0x0cU
#define DMA_DEBUG			0x20U
#define DMA_CS_ACTIVE			0x00000001U
#define DMA_CS_END			0x00000002U
#define DMA_CS_INT			0x00000004U
#define DMA_CS_ERROR			0x00000100U
#define DMA_CS_PRIORITY			(8U << 16)
#define DMA_CS_PANIC_PRIORITY		(15U << 20)
#define DMA_CS_WAIT_WRITES		0x10000000U
#define DMA_CS_RESET			0x80000000U
#define DMA_DEBUG_ERRORS		0x00000007U

/* The ARM's view of RAM on the legacy DMA's bus: the first 1 GiB at 0xc0000000. */
#define DMA_BUS_RAM			0xc0000000U
#define DMA_BUS_RAM_LIMIT		0x3fffffffU

/* The channels to try, full ones first. */
static const unsigned dma_channels[] = { 4U, 5U, 6U, 2U, 0U };

/* The control blocks' area at the start of the driver's ring buffer. */
#define AUDIO_CB_AREA			256U

/* Channel 1 (GPIO 40) is the jack's right channel. */
#define AUDIO_RIGHT_FIRST		1

/* The longest the DMA channel takes to stop after a reset. */
#define AUDIO_STOP_POLLS		1000U

/* The formats the jack plays. */
static const struct audio_format audio_formats[] = {
	{ KERN_AUDIO_FORMAT_S16_LE, 2U, 48000U, 0U },
	{ KERN_AUDIO_FORMAT_S16_LE, 2U, 44100U, 0U },
};

/* Where the hardware is, as the device tree tells it. */
struct rpi4_audio_config {
	uint64_t gpio;
	uint64_t pwm;
	uint32_t pwm_bus;
	uint64_t clock;
	uint32_t oscillator;
	uint64_t dma;
	unsigned channel;
	unsigned irq;
};

/* The jack. */
struct rpi4_audio {
	struct spinlock lock;
	struct rpi4_audio_config config;
	void *gpio;
	void *pwm;
	void *clock;
	void *dma;
	struct drv_dma_device *dma_device;
	struct drv_dma_buffer ring;
	unsigned ring_allocated;
	struct drv_audio_ops ops;
	struct drv_audio_device *audio;

	/* The prepared stream; fixed while it runs. */
	const int16_t *samples;
	uint32_t fragment_bytes;
	uint32_t fragment_count;
	uint32_t block_bytes;
	uint32_t range;
	struct rpi4_dma_cb *cbs;
	uint32_t *words;
	uint32_t cb_bus;
	uint32_t words_bus;

	/* The running state, under the lock. */
	unsigned running;
	unsigned current;
	unsigned error_logged;
};

static struct rpi4_audio board_audio;

static int describe(const struct drv_fdt *fdt, struct rpi4_audio_config *config);
static int describe_dma(const struct drv_fdt *fdt, struct rpi4_audio_config *config);
static int describe_pwm(const struct drv_fdt *fdt, struct rpi4_audio_config *config);
static int describe_clock(const struct drv_fdt *fdt, struct rpi4_audio_config *config);
static int bus_reg(const struct drv_fdt *fdt, uint32_t node, uint32_t *address);
static int attach(const struct rpi4_audio_config *config);
static void route_pins(struct rpi4_audio *audio);
static int start_clock(struct rpi4_audio *audio);
static uint32_t read_register(void *base, unsigned offset);
static void write_register(void *base, unsigned offset, uint32_t bits);
static void reset_channel(struct rpi4_audio *audio);
static void convert_block(struct rpi4_audio *audio, unsigned block);
static int audio_prepare(void *private_data, int capture, const struct audio_format *format, const struct drv_dma_buffer *buffer, uint32_t fragment_bytes, uint32_t fragment_count);
static int audio_start(void *private_data, int capture);
static void audio_stop(void *private_data, int capture);
static uint32_t audio_position(void *private_data, int capture);
static void audio_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Publishes the headphone jack from the firmware's device tree.
 */
int
drv_rpi4_audio_init(
	uint64_t fdt_physical)
{
	struct rpi4_audio_config config;
	struct drv_fdt fdt;
	const void *blob;
	bool emulated;
	int error;

	/* Maps the firmware blob through managed physical RAM. */
	blob = kern_pmem_to_kernel(fdt_physical);
	if (blob == NULL)
		return ENODEV;

	/* Opens the tree before searching it. */
	error = drv_fdt_open(&fdt, blob, RPI4_AUDIO_FDT_LIMIT);
	if (error != 0)
		return error;

	/* QEMU's raspi4b has no PWM1: the first write to it faults (T1-553, T1-554). */
	emulated = drv_rpi4_board_emulated(&fdt);
	if (emulated) {
		kern_logf("rpi4-audio: emulated board without PWM1; no headphone jack\n");
		return ENODEV;
	}

	/* Finds the four blocks the jack needs. */
	error = describe(&fdt, &config);
	if (error != 0) {
		kern_logf("rpi4-audio: no headphone jack in the device tree (%d)\n", error);
		return error;
	}

	/* Maps them and publishes /dev/dsp. */
	error = attach(&config);
	if (error != 0) {
		kern_logf("rpi4-audio: headphone jack unavailable (%d)\n", error);
		return error;
	}

	/* Succeeded: audiod can open the jack. */
	return 0;
}

/*
 * Reads the GPIO, the PWM, the clock manager and the DMA controller from
 * the tree.
 */
static int
describe(
	const struct drv_fdt *fdt,
	struct rpi4_audio_config *config)
{
	uint64_t size;
	uint32_t node;
	int error;

	/* The GPIO block, for the function of pins 40 and 41. */
	kern_memset(config, 0, sizeof(*config));
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2711-gpio", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return error;
	error = drv_fdt_reg(fdt, node, 0, &config->gpio, &size);
	if (error != 0)
		return error;
	if (size < GPIO_GPFSEL4 + 4U)
		return ERANGE;

	/* PWM1, the clock and the DMA. */
	error = describe_pwm(fdt, config);
	if (error != 0)
		return error;
	error = describe_clock(fdt, config);
	if (error != 0)
		return error;
	error = describe_dma(fdt, config);
	if (error != 0)
		return error;

	/* Succeeded: every block is known. */
	return 0;
}

/*
 * Finds PWM1 among the PWM blocks, with its address on the DMA's bus.
 */
static int
describe_pwm(
	const struct drv_fdt *fdt,
	struct rpi4_audio_config *config)
{
	uint64_t address;
	uint64_t size;
	uint32_t node;
	int error;

	/* Each PWM block, until the one at PWM1's offset. */
	node = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_fdt_find_compatible(fdt, "brcm,bcm2835-pwm", node, &node);
		if (error != 0)
			return error;
		error = drv_fdt_reg(fdt, node, 0, &address, &size);
		if (error != 0)
			return error;
		if ((address & 0xfffU) == PWM1_OFFSET)
			break;
	}

	/* Requires the whole register block. */
	if (size < PWM_REGISTERS_BYTES)
		return ERANGE;
	config->pwm = address;

	/* The untranslated address is the one the DMA uses. */
	error = bus_reg(fdt, node, &config->pwm_bus);
	if (error != 0)
		return error;

	/* Succeeded: PWM1 is known. */
	return 0;
}

/*
 * Finds the clock manager and the frequency of its first input, the
 * oscillator.
 */
static int
describe_clock(
	const struct drv_fdt *fdt,
	struct rpi4_audio_config *config)
{
	const uint8_t *value;
	uint64_t size;
	uint64_t frequency;
	uint32_t length;
	uint32_t node;
	uint32_t oscillator;
	int error;

	/* The clock manager's registers. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2711-cprman", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return error;
	error = drv_fdt_reg(fdt, node, 0, &config->clock, &size);
	if (error != 0)
		return error;
	if (size < CM_REGISTERS_BYTES)
		return ERANGE;

	/* Its first parent clock is the oscillator. */
	error = drv_fdt_property(fdt, node, "clocks", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;
	error = drv_fdt_find_phandle(fdt, (uint32_t)drv_fdt_cells_value(value, 0, 1), &oscillator);
	if (error != 0)
		return error;

	/* Its fixed frequency. */
	error = drv_fdt_property(fdt, oscillator, "clock-frequency", &value, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;
	frequency = drv_fdt_cells_value(value, 0, 1);
	if (frequency < 1000000U)
		return ERANGE;
	config->oscillator = (uint32_t)frequency;

	/* Succeeded: the clock is known. */
	return 0;
}

/*
 * Finds the legacy DMA controller, a channel the ARM may use and its
 * interrupt.
 */
static int
describe_dma(
	const struct drv_fdt *fdt,
	struct rpi4_audio_config *config)
{
	const uint8_t *value;
	uint64_t size;
	uint32_t length;
	uint32_t node;
	uint32_t mask;
	uint64_t kind;
	unsigned index;
	int error;

	/* The controller's registers. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2835-dma", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return error;
	error = drv_fdt_reg(fdt, node, 0, &config->dma, &size);
	if (error != 0)
		return error;

	/* The channels the firmware leaves to the ARM. */
	error = drv_fdt_property(fdt, node, "brcm,dma-channel-mask", &value, &length);
	if (error != 0)
		return error;
	if (length != 4U)
		return EINVAL;
	mask = (uint32_t)drv_fdt_cells_value(value, 0, 1);

	/* The first free channel of the preferred ones. */
	for (index = 0U; index < sizeof(dma_channels) / sizeof(dma_channels[0]); index++) {
		if ((mask & (1U << dma_channels[index])) != 0U)
			break;
	}

	/* None of them is free. */
	if (index == sizeof(dma_channels) / sizeof(dma_channels[0]))
		return ENODEV;
	config->channel = dma_channels[index];
	if ((uint64_t)(config->channel + 1U) * DMA_CHANNEL_STRIDE > size)
		return ERANGE;

	/* The channel's interrupt: one GIC SPI, three cells, a channel each. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if (length < (config->channel + 1U) * 12U)
		return EINVAL;
	kind = drv_fdt_cells_value(value, config->channel * 3U, 1);
	if (kind != 0U)
		return ENOTSUP;
	config->irq = (unsigned)drv_fdt_cells_value(value, config->channel * 3U + 1U, 1) + 32U;
	if (config->irq > 1019U)
		return ERANGE;

	/* Succeeded: the channel is known. */
	return 0;
}

/*
 * Reads a node's first register address as its own bus names it, before
 * any translation.
 */
static int
bus_reg(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint32_t *address)
{
	const uint8_t *value;
	uint64_t number;
	uint32_t parent;
	uint32_t cells;
	uint32_t length;
	int error;

	/* The parent bus's address width. */
	error = drv_fdt_parent(fdt, node, &parent);
	if (error != 0)
		return error;
	cells = drv_fdt_node_cells(fdt, parent, "#address-cells", 2U);
	if (cells == 0U || cells > 2U)
		return ERANGE;

	/* The first address. */
	error = drv_fdt_property(fdt, node, "reg", &value, &length);
	if (error != 0)
		return error;
	if (length < cells * 4U)
		return EINVAL;
	number = drv_fdt_cells_value(value, 0, cells);
	if (number > UINT32_MAX)
		return ERANGE;
	*address = (uint32_t)number;

	/* Succeeded: the bus address. */
	return 0;
}

/*
 * Maps the blocks, routes the pins, starts the clock and publishes the
 * backend.
 */
static int
attach(
	const struct rpi4_audio_config *config)
{
	struct drv_dma_constraints constraints;
	struct rpi4_audio *audio;
	int error;

	/* Refuses a second attachment. */
	audio = &board_audio;
	if (audio->gpio != NULL)
		return EALREADY;
	audio->config = *config;
	spin_init(&audio->lock, LOCK_RANK_DEVICE, "rpi4 audio");

	/* Maps the four blocks; a failure leaves them mapped, as a permanent board device would. */
	error = kern_device_map(config->gpio, GPIO_GPFSEL4 + 4U, KERN_DEVICE_UNCACHED, &audio->gpio);
	if (error != 0)
		return error;
	error = kern_device_map(config->pwm, PWM_REGISTERS_BYTES, KERN_DEVICE_UNCACHED, &audio->pwm);
	if (error != 0)
		return error;
	error = kern_device_map(config->clock, CM_REGISTERS_BYTES, KERN_DEVICE_UNCACHED, &audio->clock);
	if (error != 0)
		return error;
	error = kern_device_map(config->dma + config->channel * DMA_CHANNEL_STRIDE, DMA_CHANNEL_STRIDE,
				KERN_DEVICE_UNCACHED, &audio->dma);
	if (error != 0)
		return error;

	/* Leaves the channel idle before its interrupt is bound. */
	reset_channel(audio);

	/* The pins and the PWM's clock. */
	route_pins(audio);
	error = start_clock(audio);
	if (error != 0)
		return error;

	/* DMA memory in the first 1 GiB, at its bus alias, uncached. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 32U;
	constraints.max_segment_size = 1024U * 1024U;
	constraints.coherent = 0;
	error = drv_dma_device_create_window(&constraints, DMA_BUS_RAM, DMA_BUS_RAM_LIMIT, &audio->dma_device);
	if (error != 0)
		return error;

	/* The channel's interrupt. */
	kern_irq_mask((int)config->irq);
	error = kern_irq_set_mode((int)config->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		return error;
	error = kern_irq_register((int)config->irq, audio_interrupt, audio);
	if (error != 0)
		return error;

	/* The backend. */
	audio->ops.playback = 1U;
	audio->ops.capture = 0U;
	audio->ops.formats = audio_formats;
	audio->ops.format_count = sizeof(audio_formats) / sizeof(audio_formats[0]);
	audio->ops.prepare = audio_prepare;
	audio->ops.start = audio_start;
	audio->ops.stop = audio_stop;
	audio->ops.position = audio_position;
	error = drv_audio_register(&audio->ops, audio, audio->dma_device, &audio->audio);
	if (error != 0)
		return error;

	/* Succeeded: the jack is published. */
	kern_logf("rpi4-audio: PWM1 at %llx, DMA channel %u, irq %u, clock %u Hz\n",
		  (unsigned long long)config->pwm, config->channel, config->irq, config->oscillator);
	return 0;
}

/*
 * Gives GPIO 40 and 41 to PWM1.
 */
static void
route_pins(
	struct rpi4_audio *audio)
{
	uint32_t select;
	unsigned pin;
	unsigned shift;

	/* Changes only the two pins' fields of the shared register. */
	select = read_register(audio->gpio, GPIO_GPFSEL4);
	for (pin = GPIO_JACK_FIRST; pin <= GPIO_JACK_LAST; pin++) {
		shift = (pin - GPIO_JACK_FIRST) * 3U;
		select &= ~(7U << shift);
		select |= GPIO_FUNCTION_ALT0 << shift;
	}

	/* Writes both back at once. */
	write_register(audio->gpio, GPIO_GPFSEL4, select);
}

/*
 * Runs the PWM's clock from the oscillator, through CM_PWM_DIVISOR.
 */
static int
start_clock(
	struct rpi4_audio *audio)
{
	uint32_t status;
	unsigned polls;

	/* Stops the clock and waits for it to settle before changing it. */
	write_register(audio->clock, CM_PWMCTL, CM_PASSWORD | CM_SOURCE_OSCILLATOR);
	for (polls = 0U; polls < AUDIO_STOP_POLLS; polls++) {
		status = read_register(audio->clock, CM_PWMCTL);
		if ((status & CM_BUSY) == 0U)
			break;
		kern_usleep_range(10U, 20U);
	}

	/* A clock that never settles is left alone. */
	if (polls == AUDIO_STOP_POLLS)
		return ETIMEDOUT;

	/* Sets the divisor and starts it. */
	write_register(audio->clock, CM_PWMDIV, CM_PASSWORD | (CM_PWM_DIVISOR << CM_DIVI_SHIFT));
	write_register(audio->clock, CM_PWMCTL, CM_PASSWORD | CM_SOURCE_OSCILLATOR);
	write_register(audio->clock, CM_PWMCTL, CM_PASSWORD | CM_SOURCE_OSCILLATOR | CM_ENABLE);

	/* Succeeded: the PWM counts at the divided oscillator's rate. */
	return 0;
}

/* Reads a register of a mapped block. */
static uint32_t
read_register(
	void *base,
	unsigned offset)
{
	uint32_t bits;

	/* Reads the ordered register. */
	bits = kern_mmio_read32((const uint8_t *)base + offset);

	/* Succeeded: the register's value. */
	return bits;
}

/* Writes a register of a mapped block. */
static void
write_register(
	void *base,
	unsigned offset,
	uint32_t bits)
{
	/* Writes the ordered register. */
	kern_mmio_write32((uint8_t *)base + offset, bits);
}

/*
 * Stops the DMA channel and waits until it no longer reads.
 */
static void
reset_channel(
	struct rpi4_audio *audio)
{
	uint32_t status;
	unsigned polls;

	/* Pauses the channel, then resets it. */
	write_register(audio->dma, DMA_CS, 0U);
	write_register(audio->dma, DMA_CS, DMA_CS_RESET);
	for (polls = 0U; polls < AUDIO_STOP_POLLS; polls++) {
		status = read_register(audio->dma, DMA_CS);
		if ((status & DMA_CS_ACTIVE) == 0U)
			break;
		kern_usleep_range(10U, 20U);
	}

	/* Clears what the last transfer left. */
	write_register(audio->dma, DMA_CS, DMA_CS_INT | DMA_CS_END);
	write_register(audio->dma, DMA_DEBUG, DMA_DEBUG_ERRORS);
}

/*
 * Converts one fragment of the framework's ring into its block of duty
 * words.
 */
static void
convert_block(
	struct rpi4_audio *audio,
	unsigned block)
{
	const int16_t *samples;
	uint32_t *words;

	/* The fragment and its block. */
	samples = audio->samples + (size_t)block * (audio->fragment_bytes / 2U);
	words = audio->words + (size_t)block * (audio->fragment_bytes / 2U);

	/* A fragment of 4-byte frames. */
	rpi4_audio_convert(samples, words, audio->fragment_bytes / 4U, audio->range, AUDIO_RIGHT_FIRST);
}

/*
 * Prepares a playback stream: the PWM's range for the rate, the duty
 * words and the chain of control blocks.
 */
static int
audio_prepare(
	void *private_data,
	int capture,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count)
{
	struct rpi4_audio *audio;
	size_t total;
	unsigned block;
	int error;

	/* Playback only, 16-bit stereo. */
	audio = private_data;
	if (capture != 0)
		return ENOTSUP;
	if (format->format != KERN_AUDIO_FORMAT_S16_LE || format->channels != 2U || format->rate == 0U)
		return EINVAL;

	/* Whole frames, at least three fragments, blocks a DMA lite channel can move. */
	if (fragment_bytes == 0U || fragment_bytes % 4U != 0U || fragment_bytes > 0x7ffcU)
		return EINVAL;
	if (fragment_count < 3U || fragment_count * sizeof(struct rpi4_dma_cb) > AUDIO_CB_AREA)
		return EINVAL;
	if (buffer->size < (size_t)fragment_bytes * fragment_count)
		return EINVAL;

	/* The control blocks, then the duty words; a larger stream gets a new buffer. */
	total = AUDIO_CB_AREA + (size_t)fragment_bytes * 2U * fragment_count;
	if (audio->ring_allocated != 0U && audio->ring.size < total) {
		drv_dma_free_coherent(audio->dma_device, &audio->ring);
		audio->ring_allocated = 0U;
	}

	/* Allocates the buffer once, or again after a free. */
	if (audio->ring_allocated == 0U) {
		error = drv_dma_alloc_coherent(audio->dma_device, total, AUDIO_CB_AREA, &audio->ring);
		if (error != 0)
			return error;
		audio->ring_allocated = 1U;
	}

	/* The stream. */
	audio->samples = buffer->address;
	audio->fragment_bytes = fragment_bytes;
	audio->fragment_count = fragment_count;
	audio->block_bytes = fragment_bytes * 2U;
	audio->range = (audio->config.oscillator / CM_PWM_DIVISOR + format->rate / 2U) / format->rate;
	audio->cbs = audio->ring.address;
	audio->words = (uint32_t *)((uint8_t *)audio->ring.address + AUDIO_CB_AREA);
	audio->cb_bus = (uint32_t)audio->ring.device_address;
	audio->words_bus = audio->cb_bus + AUDIO_CB_AREA;

	/* The chain and every block from the ring as it is now. */
	rpi4_audio_chain(audio->cbs, fragment_count, audio->cb_bus, audio->words_bus, audio->block_bytes,
			 audio->config.pwm_bus + PWM_FIF1);
	for (block = 0U; block < fragment_count; block++)
		convert_block(audio, block);

	/* Succeeded: the stream can start. */
	return 0;
}

/*
 * Starts the PWM with its DMA requests, then the DMA channel at the first
 * block.
 */
static int
audio_start(
	void *private_data,
	int capture)
{
	struct rpi4_audio *audio;
	unsigned long enabled;

	/* Playback only, after a prepare. */
	audio = private_data;
	if (capture != 0)
		return ENOTSUP;
	if (audio->cbs == NULL)
		return EINVAL;

	/* Brings the stream's first fragments up to date. */
	convert_block(audio, 0U);
	convert_block(audio, 1U);

	/* The PWM: stopped, cleared, the range, DMA requests, then both channels from the FIFO. */
	write_register(audio->pwm, PWM_CTL, 0U);
	write_register(audio->pwm, PWM_STA, PWM_STA_ERRORS);
	write_register(audio->pwm, PWM_CTL, PWM_CTL_CLRF);
	kern_usleep_range(10U, 20U);
	write_register(audio->pwm, PWM_RNG1, audio->range);
	write_register(audio->pwm, PWM_RNG2, audio->range);
	write_register(audio->pwm, PWM_DMAC, PWM_DMAC_ENAB | PWM_DMAC_THRESHOLDS);
	write_register(audio->pwm, PWM_CTL, PWM_CTL_PWEN1 | PWM_CTL_USEF1 | PWM_CTL_PWEN2 | PWM_CTL_USEF2);

	/* The DMA, from the first block. */
	enabled = spin_lock_irqsave(&audio->lock);
	audio->current = 0U;
	audio->running = 1U;
	reset_channel(audio);
	write_register(audio->dma, DMA_CONBLK_AD, audio->cb_bus);
	write_register(audio->dma, DMA_CS,
		       DMA_CS_ACTIVE | DMA_CS_PRIORITY | DMA_CS_PANIC_PRIORITY | DMA_CS_WAIT_WRITES);
	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Unmasks the channel's interrupt. */
	kern_irq_unmask((int)audio->config.irq);

	/* Succeeded: the jack plays. */
	return 0;
}

/*
 * Stops the DMA and the PWM.  The DMA reads only the driver's own words;
 * once running is clear the interrupt no longer reads the framework's ring.
 */
static void
audio_stop(
	void *private_data,
	int capture)
{
	struct rpi4_audio *audio;
	unsigned long enabled;

	/* Playback only. */
	audio = private_data;
	if (capture != 0)
		return;

	/* Marks the stream stopped and stops the channel. */
	enabled = spin_lock_irqsave(&audio->lock);
	audio->running = 0U;
	reset_channel(audio);
	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Then the PWM's requests and outputs. */
	write_register(audio->pwm, PWM_DMAC, 0U);
	write_register(audio->pwm, PWM_CTL, 0U);
}

/*
 * Tells where the DMA reads, in the framework's ring.
 */
static uint32_t
audio_position(
	void *private_data,
	int capture)
{
	struct rpi4_audio *audio;
	uint32_t source;

	/* Playback only. */
	audio = private_data;
	if (capture != 0 || audio->cbs == NULL)
		return 0U;

	/* The DMA's source address, as a sample offset. */
	source = read_register(audio->dma, DMA_SOURCE_AD);

	/* Succeeded: the position. */
	return rpi4_audio_position(source, audio->words_bus, audio->block_bytes, audio->fragment_count);
}

/*
 * Handles a block's end: converts the block the DMA has entered and the
 * next, and reports each fragment the DMA has finished since the last
 * interrupt.
 */
static void
audio_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct rpi4_audio *audio;
	unsigned long enabled;
	unsigned block;
	unsigned finished;
	uint32_t status;
	uint32_t source;

	/* The acknowledgement token identifies the delivery. */
	(void)irq;
	audio = argument;
	finished = 0U;

	/* Clears the channel's interrupt, keeping a running channel active. */
	enabled = spin_lock_irqsave(&audio->lock);
	status = read_register(audio->dma, DMA_CS);
	if (audio->running == 0U) {
		write_register(audio->dma, DMA_CS, DMA_CS_INT | DMA_CS_END);
		spin_unlock_irqrestore(&audio->lock, enabled);
		kern_irq_send_eoi(acknowledge);
		return;
	}

	/* A running channel is kept active. */
	write_register(audio->dma, DMA_CS,
		       DMA_CS_INT | DMA_CS_END | DMA_CS_ACTIVE | DMA_CS_PRIORITY | DMA_CS_PANIC_PRIORITY |
			       DMA_CS_WAIT_WRITES);

	/* Reports a DMA error once a stream. */
	if ((status & DMA_CS_ERROR) != 0U && audio->error_logged == 0U) {
		audio->error_logged = 1U;
		kern_logf("rpi4-audio: DMA error, CS %08x debug %08x\n", status,
			  read_register(audio->dma, DMA_DEBUG));
	}

	/* The block the DMA is in, the fragments behind it and the blocks to refresh. */
	source = read_register(audio->dma, DMA_SOURCE_AD);
	block = rpi4_audio_fragment(source, audio->words_bus, audio->block_bytes, audio->fragment_count);
	if (block < audio->fragment_count) {
		finished = (block + audio->fragment_count - audio->current) % audio->fragment_count;
		audio->current = block;
		convert_block(audio, block);
		convert_block(audio, (block + 1U) % audio->fragment_count);
	}

	/* Releases the stream before the framework's lock is taken. */
	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Completes the level acknowledgement before the framework's work. */
	kern_irq_send_eoi(acknowledge);
	while (finished > 0U) {
		drv_audio_interrupt(audio->audio, 0);
		finished--;
	}
}
