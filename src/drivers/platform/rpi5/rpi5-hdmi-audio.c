/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's HDMI audio.
 *
 * The firmware brings up the HDMI port the monitor is on before the kernel
 * starts and keeps the mode; the display driver never touches the HDMI
 * block.  The audio path reuses what the firmware set up: a port's audio
 * FIFO (in the block the two ports share), the port's packet and clock
 * regeneration registers, the PHY whose rate gives the TMDS clock, a DMA
 * channel, and the gate of the port's audio clock in the clock and reset
 * block.  The driver writes only the audio registers of the port it chose,
 * the audio InfoFrame's slot of the packet RAM, the port's audio clock gate
 * and its own DMA channel; everything else the firmware set is only read.
 *
 * A port's registers answer only while the port's clocks run and its reset
 * is released; an access to a stopped port can raise an SError or stall
 * the bus.  So the clock and reset block and the firmware's account of the
 * HDMI clocks are read first, and a port is read only when its reset is
 * released and the firmware says its clocks run.  At boot each step is
 * followed by a check for an SError the step left pending, and the
 * registers read are logged once, in lines that begin with
 * "rpi5-hdmi-audio: probe".
 *
 * The first port that is out of reset, has its clocks running, sends HDMI
 * rather than DVI and sees a sink is offered to the audio core as the one
 * playback device, at 16-bit stereo 48 kHz.  The DMA reads a ring of IEC
 * 60958 subframes the driver converts from the core's ring: at each
 * fragment's interrupt the fragment the DMA has entered and the next one
 * are converted again, after the core has copied what the writer wrote.
 *
 * A thread watches a running stream every 10 ms.  It checks, without
 * touching a port whose clocks may have stopped, that the port still takes
 * sound and still holds what the driver wrote, and writes it again when the
 * firmware changed it.  When the port stops taking sound (the sink went
 * away, the firmware stopped its clocks, or the DMA stopped moving), the
 * thread stops the DMA and moves the stream on by the clock, dropping the
 * sound, so that writers and drains do not stall; it goes back to the DMA
 * once the port takes sound again.  A boot without a sink offers no device.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/audio/audio.h>
#include <drivers/generic/dma.h>
#include <drivers/generic/fdt.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/audio.h>
#include <uapi/errno.h>

#include "drivers/platform/rpi5/rpi5-firmware.h"
#include "drivers/platform/rpi5/rpi5-hdmi-audio.h"
#include "drivers/platform/rpi5/rpi5-hdmi-audio-wire.h"
#include "drivers/platform/rpi5/rpi5-serror.h"

/* How much of the device tree blob the driver may read. */
#define HDMI_AUDIO_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/* The BCM2712's HDMI ports. */
#define HDMI_AUDIO_PORTS		2U

/* The most register words one log line of a port carries besides its named ones. */
#define PORT_LOG_WORDS			8U

/* The longest property string the probe prints. */
#define HDMI_AUDIO_NAME_LIMIT		64U

/*
 * The port's own way of regenerating the audio clock: the port measures
 * CTS itself, with N 6144.  The other way, the driver's CTS with the
 * table's N, is chosen by false.
 */
#define HDMI_AUDIO_HARDWARE_CTS		true

/*
 * The clock and reset block of the HDMI ports.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/clk/bcm/clk-bcm2711-dvp.c (the reset and clock-gate register
 * offsets, the gates of clocks 0 and 1 at bits 3 and 4, a set bit stops
 * the clock), drivers/reset/reset-simple.c (a set reset bit holds the
 * line in reset).
 */
#define DVP_RESET			0x04U
#define DVP_GATE			0x08U
#define DVP_GATE_FIRST_BIT		3U
#define DVP_GATE_COUNT			2U
#define DVP_WINDOW_BYTES		0x10U

/*
 * The registers of one port's HDMI core, its PHY and its rate manager.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi_regs.h (vc6_hdmi_hdmi0_fields and
 * vc6_hdmi_hdmi1_fields: the core's audio, packet, scheduler, timing,
 * deep colour and hotplug registers, the PHY's reset, power, reference
 * and VCO divider registers, and the rate manager's control, offset and
 * format registers).
 */
#define CORE_CHANNEL_MAP		0x0a4U
#define CORE_AUDIO_CONFIG		0x0a8U
#define CORE_AUDIO_PACKET		0x0c0U
#define CORE_PACKET_CONFIG		0x0c4U
#define CORE_PACKET_STATUS		0x0ccU
#define CORE_CLOCK_REGEN		0x0d0U
#define CORE_CTS_0			0x0d4U
#define CORE_CTS_1			0x0d8U
#define CORE_SCHEDULER			0x0e8U
#define CORE_HORIZONTAL_A		0x0ecU
#define CORE_HORIZONTAL_B		0x0f0U
#define CORE_VERTICAL_A0		0x0f4U
#define CORE_VERTICAL_B0		0x0f8U
#define CORE_VERTICAL_A1		0x100U
#define CORE_VERTICAL_B1		0x104U
#define CORE_MISC			0x114U
#define CORE_DEEP_COLOUR		0x18cU
#define CORE_HOTPLUG			0x1c8U
#define PHY_RESET			0x000U
#define PHY_POWER			0x004U
#define PHY_REFERENCE			0x01cU
#define PHY_VCO_DIVIDER			0x02cU
#define RATE_CONTROL			0x000U
#define RATE_OFFSET			0x018U
#define RATE_FORMAT			0x01cU

/*
 * The audio FIFO registers of each port in the block the ports share.  The
 * FIFO's data register is never read: a read takes a word out of it.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi_regs.h (vc6_hdmi_hdmi0_fields and
 * vc6_hdmi_hdmi1_fields: the audio FIFO's control, threshold, format,
 * data and sample-rate registers of each port).
 */
#define FIFO_CONTROL_PORT0		0x10U
#define FIFO_CONTROL_PORT1		0x30U
#define FIFO_CONTROL			0x00U
#define FIFO_THRESHOLD			0x04U
#define FIFO_FORMAT			0x08U
#define FIFO_DATA			0x0cU
#define FIFO_SAMPLE_RATE		0x10U

/*
 * The bits the port's choice and the InfoFrame's slot use.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the scheduler's HDMI mode bit, the
 * hotplug register's connected bit, the packet RAM's global enable and
 * its per-slot enable and busy bits).
 */
#define SCHEDULER_HDMI			0x00000001U
#define HOTPLUG_CONNECTED		0x00000001U
#define PACKET_CONFIG_ENABLE		0x00010000U

/*
 * How long the packet RAM may take to take a slot in or out of service,
 * and how often it is looked at meanwhile.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (the 100 ms waits of
 * vc4_hdmi_stop_packet and vc4_hdmi_write_infoframe).
 */
#define PACKET_WAIT_POLLS		100U
#define PACKET_WAIT_MIN_US		1000U
#define PACKET_WAIT_MAX_US		2000U

/*
 * The compositor's version register, whose low byte tells the stepping.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_regs.h (the HVS's version register).
 */
#define HVS_VERSION			0x000U
#define HVS_WINDOW_BYTES		0x100U

/*
 * The 40-bit DMA channels: each channel's registers, one channel every
 * 0x100 bytes, the first channel's number in bits 8 to 11 of the node's
 * physical address, the first 40-bit channel of the BCM2712, and the
 * channel status and debug bits the driver uses to stop a channel.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (the 40-bit channel's CS, CB, DEBUG, TI, SRC
 * and SRCI registers, the channel stride, the first channel from the base
 * address, the BCM2712's 40-bit channels, the CS active, interrupt, error
 * and outstanding-transactions bits, the DEBUG reset bit).
 */
#define DMA_CHANNEL_STRIDE		0x100U
#define DMA_CHANNEL_SHIFT		8U
#define DMA_CHANNEL_MASK		0x0fU
#define DMA_STATUS			0x00U
#define DMA_BLOCK			0x04U
#define DMA_DEBUG			0x0cU
#define DMA_TRANSFER			0x10U
#define DMA_SOURCE			0x14U
#define DMA_SOURCE_INFO			0x18U
#define DMA_CHANNEL_LIMIT		16U
#define DMA_WIDE_FIRST			6U
#define DMA_STATUS_ERROR		0x00000400U
#define DMA_STATUS_TRANSACTIONS		0x02000000U
#define DMA_DEBUG_RESET			0x00800000U

/*
 * How many times the channel's status is read while it finishes its
 * outstanding transactions, and while its reset clears it.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (the bounded wait of bcm2835_dma_abort).
 */
#define DMA_STOP_POLLS			100U

/* The control blocks' area at the start of the driver's buffer, and the buffer's alignment. */
#define WIRE_CB_AREA			256U
#define WIRE_ALIGNMENT			256U
#define WIRE_RING_BYTES			(DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES * DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT)

/* The DMA's reach: 40-bit addresses, one 4 GiB window per ring. */
#define WIRE_ADDRESS_BITS		40U
#define WIRE_SEGMENT_BYTES		(1024U * 1024U)
#define WIRE_SEGMENT_BOUNDARY		(1ULL << 32)

/* The interrupt controller's shared lines: three cells, kind 0, numbered after the 32 private ones. */
#define GIC_CELLS			3U
#define GIC_KIND_SPI			0U
#define GIC_SPI_BASE			32U
#define GIC_SPI_LIMIT			988U

/*
 * The watching thread's timing: its period, how often it asks the firmware
 * about the clocks, how long the DMA may stand still (two fragments), and
 * how long it waits before trying the DMA again after a failure.
 */
#define WATCH_PERIOD_MS			10U
#define WATCH_CLOCK_MS			100U
#define WATCH_STALL_MS			43U
#define WATCH_RETRY_MS			5000U

/* The most changes of clock a stream logs. */
#define WATCH_LOG_LIMIT			8U

/*
 * Set-ups of the port that come this soon after the last one count as a
 * run; a run this long means the port does not read back what was
 * written, and the thread stops comparing.
 */
#define WATCH_REWRITE_MS		1000U
#define WATCH_REWRITE_LIMIT		3U

/*
 * The firmware's clock questions and the bits of a clock's state.
 *
 * Values referenced: the Raspberry Pi firmware wiki's mailbox property
 * interface (get clock state, get clock rate; state bit 0 on, bit 1 the
 * clock does not exist).
 */
#define FIRMWARE_CLOCK_STATE		0x00030001U
#define FIRMWARE_CLOCK_RATE		0x00030002U
#define FIRMWARE_CLOCK_ON		0x00000001U
#define FIRMWARE_CLOCK_ABSENT		0x00000002U

/*
 * What the firmware says of a clock: it runs, it is stopped or absent, or
 * the driver could not ask.
 */
enum clock_answer {
	CLOCK_UNKNOWN = 0,
	CLOCK_OFF,
	CLOCK_ON
};

/*
 * What moves a running stream on: nothing (stopped), the DMA reading the
 * subframe ring, or the clock standing in for it while no port takes the
 * sound.
 */
enum stream_mode {
	STREAM_IDLE = 0,
	STREAM_DMA,
	STREAM_CLOCK
};

/*
 * One register window the driver maps, as the device tree names it.
 *
 * mapped is NULL while the window is not mapped; a mapped window stays
 * mapped for as long as the kernel runs.
 */
struct hdmi_audio_window {
	uint64_t physical;
	uint64_t size;
	volatile uint8_t *mapped;
};

/*
 * One HDMI port: where the device tree puts it, what it ties it to, and
 * the registers the choice of a port reads.
 */
struct hdmi_audio_port {
	bool found;
	uint32_t node;
	bool tree_enabled;
	bool dma_found;
	uint32_t dma_node;
	uint32_t dma_cell;
	bool reset_found;
	uint32_t reset_bit;
	bool audio_clock_found;
	uint32_t audio_clock;
	struct hdmi_audio_window core;
	struct hdmi_audio_window phy;
	struct hdmi_audio_window rate;
	struct hdmi_audio_window shared;
	struct hdmi_audio_window packet;
	bool read;
	uint32_t scheduler;
	uint32_t hotplug;
};

/*
 * What the port held after the driver last set it up: the values the
 * watching thread compares the port with.
 */
struct hdmi_audio_written {
	uint64_t tmds_hz;
	uint32_t regen;
	uint32_t cts;
	uint32_t sample;
	uint32_t format;
	uint32_t threshold;
	bool infoframe;
};

/*
 * The HDMI audio path and its stream.
 *
 * One instance.  What attach fills (the tree, the ports, the chosen port,
 * the DMA channel, the buffers) is written on the boot thread before the
 * device is offered and only read afterwards.  The control mutex orders
 * prepare, start, stop and the watching thread, and guards the port state
 * (written, programmed, the cached clock answer, the timers).  The spin
 * lock, taken inside the mutex and by the interrupt, guards every write to
 * the port and to the DMA channel and the stream state (pcm, running,
 * mode, current, serial, position, the stand-in clock); mode and position
 * are also read without it by position().
 */
struct hdmi_audio {
	/* Found at boot. */
	struct drv_fdt fdt;
	struct hdmi_audio_port ports[HDMI_AUDIO_PORTS];
	struct hdmi_audio_window dvp;
	bool dvp_read;
	uint32_t dvp_reset;
	uint32_t dvp_gate;
	uint64_t audio_parent_hz;
	uint32_t hvs_version;
	enum drv_rpi5_hdmi_audio_stepping stepping;
	bool clocks_known;
	bool clocks_on;
	bool clock_ids_found;
	uint32_t hdmi_clock;
	uint32_t bvb_clock;

	/* The port and the DMA channel the stream uses, fixed once offered. */
	uint32_t port;
	struct drv_rpi5_hdmi_audio_dma_request request;
	bool dreq_agrees;
	uint32_t run_word;
	uint32_t sample_word;
	uint32_t threshold_word;
	uint64_t fifo_bus;
	uint32_t channel;
	int irq;
	struct hdmi_audio_window channel_window;
	struct drv_dma_device *dma_device;
	struct drv_dma_buffer wire;
	struct drv_rpi5_hdmi_audio_dma_cb *cbs;
	uint32_t *words;
	uint64_t cb_bus;
	uint64_t data_bus;
	uint8_t status[DRV_RPI5_HDMI_AUDIO_STATUS_BYTES];
	struct drv_audio_ops ops;
	struct drv_audio_device *device;
	struct thread *thread;
	struct mutex control;
	struct spinlock lock;
	struct wait_queue wake;

	/* The port's state, under the control mutex. */
	struct hdmi_audio_written written;
	bool programmed;
	bool clock_alive;
	bool clock_asked;
	uint64_t clock_tick;
	uint64_t retry_tick;
	uint32_t last_source;
	uint64_t last_move_tick;
	uint32_t fifo_errors;
	unsigned changes_logged;
	uint64_t logged_tmds_hz;
	bool compare;
	unsigned rewrites;
	uint64_t rewrite_tick;

	/* The stream's state, under the spin lock. */
	const int16_t *pcm;
	bool prepared;
	bool running;
	volatile unsigned mode;
	volatile unsigned position;
	uint32_t current;
	uint64_t serial;
	uint64_t clock_base;
	uint64_t clock_done;
	unsigned dma_errors;
};

/*
 * The HDMI audio path the boot found.
 *
 * Zero until drv_rpi5_hdmi_audio_init() runs; see struct hdmi_audio for
 * what guards each part afterwards.
 */
static struct hdmi_audio hdmi_audio;

/* The device tree names of the two ports. */
static const char *const port_compatible[HDMI_AUDIO_PORTS] = {
	"brcm,bcm2712-hdmi0",
	"brcm,bcm2712-hdmi1"
};

/* The steps the SError check names after reading each port. */
static const char *const port_step[HDMI_AUDIO_PORTS] = {
	"rpi5-hdmi-audio: reading HDMI0",
	"rpi5-hdmi-audio: reading HDMI1"
};

/* Where each port's audio FIFO registers start in the shared block. */
static const uint32_t port_fifo[HDMI_AUDIO_PORTS] = {
	FIFO_CONTROL_PORT0,
	FIFO_CONTROL_PORT1
};

/* The one format the port plays: 16-bit stereo at 48 kHz. */
static const struct audio_format hdmi_audio_formats[] = {
	{ KERN_AUDIO_FORMAT_S16_LE, DRV_RPI5_HDMI_AUDIO_CHANNELS, DRV_RPI5_HDMI_AUDIO_RATE, 0U },
};

static void report_tree(const struct drv_fdt *fdt);
static void report_string(const struct drv_fdt *fdt, uint32_t node, const char *property, const char *label);
static int find_port(struct hdmi_audio *audio, uint32_t index);
static int find_entry(const struct drv_fdt *fdt, uint32_t node, const char *list, const char *cells, unsigned wanted, uint32_t *provider, uint32_t *argument);
static int map_named(const struct drv_fdt *fdt, uint32_t node, const char *name, unsigned attributes, struct hdmi_audio_window *window);
static int map_window(uint64_t physical, uint64_t size, unsigned attributes, struct hdmi_audio_window *window);
static void read_stepping(struct hdmi_audio *audio);
static void read_dvp(struct hdmi_audio *audio);
static void read_port_clocks(struct hdmi_audio *audio);
static int find_clock(const struct drv_fdt *fdt, uint32_t node, const char *name, uint32_t *clock);
static enum clock_answer firmware_clock(const struct drv_fdt *fdt, uint32_t node, const char *name, const char *label);
static enum clock_answer ask_clock(const struct drv_fdt *fdt, uint32_t clock);
static const char *port_unreadable(const struct hdmi_audio *audio, const struct hdmi_audio_port *port);
static void read_port(struct hdmi_audio *audio, uint32_t index);
static void report_rate(const struct hdmi_audio *audio, uint32_t index, uint32_t offset, uint32_t reference, uint32_t divider);
static void read_dma(const struct hdmi_audio *audio);
static int choose_port(const struct hdmi_audio *audio, uint32_t *chosen);
static int attach_stream(struct hdmi_audio *audio, uint32_t index);
static int prepare_stream_hardware(struct hdmi_audio *audio, struct hdmi_audio_port *port);
static int choose_channel(struct hdmi_audio *audio, uint32_t node);
static int find_channel_interrupt(struct hdmi_audio *audio, uint32_t node, uint32_t channel);
static bool bus_identity(const struct drv_fdt *fdt, uint32_t node, uint64_t address, uint64_t bytes);
static int start_service(struct hdmi_audio *audio);
static bool read_tmds(const struct hdmi_audio *audio, uint64_t *tmds_hz);
static bool port_alive(struct hdmi_audio *audio, bool ask_now);
static bool program_port(struct hdmi_audio *audio);
static void write_infoframe(struct hdmi_audio *audio);
static bool wait_packet(const struct hdmi_audio_port *port, uint32_t slot_bit, bool busy);
static void quiet_port(struct hdmi_audio *audio);
static bool port_changed(struct hdmi_audio *audio);
static void start_dma_locked(struct hdmi_audio *audio, uint32_t fragment);
static void halt_dma_locked(struct hdmi_audio *audio);
static uint32_t account_dma_locked(struct hdmi_audio *audio);
static void convert_fragment(struct hdmi_audio *audio, uint32_t fragment, uint64_t serial);
static void refresh_locked(struct hdmi_audio *audio);
static void report_fragments(struct hdmi_audio *audio, uint32_t count);
static void enter_clock(struct hdmi_audio *audio, const char *reason);
static bool enter_dma(struct hdmi_audio *audio);
static void run_clock(struct hdmi_audio *audio);
static bool dma_moving(struct hdmi_audio *audio, uint64_t now);
static void watch_once(struct hdmi_audio *audio);
static void watch_thread(void *argument);
static int audio_prepare(void *private_data, int capture, const struct audio_format *format, const struct drv_dma_buffer *buffer, uint32_t fragment_bytes, uint32_t fragment_count);
static int audio_start(void *private_data, int capture);
static void audio_stop(void *private_data, int capture);
static uint32_t audio_position(void *private_data, int capture);
static void audio_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static uint32_t read_register(const struct hdmi_audio_window *window, uint32_t offset);
static void write_register(const struct hdmi_audio_window *window, uint32_t offset, uint32_t value);

/*
 * Finds the HDMI audio path in the device tree, logs what the firmware
 * left in it, and offers the port that takes sound to the audio core.
 *
 * Returns 0 when a port was offered, ENODEV when none could carry audio,
 * or the error that kept the tree from being read or the port from being
 * offered.
 */
int
drv_rpi5_hdmi_audio_init(
	uint64_t fdt_physical)
{
	struct hdmi_audio *audio;
	const void *blob;
	uint32_t index;
	uint32_t chosen;
	unsigned found;
	int error;

	audio = &hdmi_audio;

	/* Opens the device tree through the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_physical);
	if (fdt_physical == 0 || blob == NULL) {
		kern_logf("rpi5-hdmi-audio: no device tree\n");
		return ENODEV;
	}
	error = drv_fdt_open(&audio->fdt, blob, HDMI_AUDIO_FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: unreadable device tree (%d)\n", error);
		return error;
	}

	/* Names the board and the stepping the tree was made for. */
	report_tree(&audio->fdt);

	/* Finds the two ports; a board without either has no HDMI audio. */
	found = 0;
	for (index = 0; index < HDMI_AUDIO_PORTS; index++) {
		error = find_port(audio, index);
		if (error == 0)
			found++;
	}

	/* Refuses a tree that names no HDMI port. */
	if (found == 0) {
		kern_logf("rpi5-hdmi-audio: probe no HDMI port in the device tree\n");
		return ENODEV;
	}

	/* Reads the chip's stepping from the compositor, when its clocks run. */
	read_stepping(audio);
	(void)drv_rpi5_serror_check("rpi5-hdmi-audio: reading the stepping");

	/* Reads the ports' resets and audio clock gates. */
	read_dvp(audio);
	(void)drv_rpi5_serror_check("rpi5-hdmi-audio: reading the clock and reset block");

	/* Asks the firmware whether the HDMI clocks run. */
	read_port_clocks(audio);

	/* Reads each port the checks above show to be safe to read. */
	for (index = 0; index < HDMI_AUDIO_PORTS; index++)
		read_port(audio, index);

	/* Reads the DMA channels, to see which ones are in use. */
	read_dma(audio);
	(void)drv_rpi5_serror_check("rpi5-hdmi-audio: reading the DMA channels");

	/* Names the port that could carry audio. */
	error = choose_port(audio, &chosen);
	if (error != 0)
		return error;

	/* Offers it to the audio core. */
	error = attach_stream(audio, chosen);
	(void)drv_rpi5_serror_check("rpi5-hdmi-audio: offering the port");
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: hdmi%u not offered (%d)\n", chosen, error);
		return error;
	}

	/* Succeeded: the port plays through /dev/dsp. */
	return 0;
}

/* Logs the tree's board name, its compatible list's first entry and the GPU's. */
static void
report_tree(
	const struct drv_fdt *fdt)
{
	uint32_t node;
	int error;

	/* The root node's model and compatible. */
	report_string(fdt, fdt->structure_offset, "model", "model");
	report_string(fdt, fdt->structure_offset, "compatible", "board");

	/* The GPU node's first compatible names the stepping the tree was made for. */
	error = drv_fdt_find_compatible(fdt, "brcm,bcm2712-vc6", DRV_FDT_NO_NODE, &node);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe tree gpu none (%d)\n", error);
		return;
	}
	report_string(fdt, node, "compatible", "gpu");
}

/* Logs the first string of a string property, when it is one. */
static void
report_string(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *property,
	const char *label)
{
	const uint8_t *value;
	char text[HDMI_AUDIO_NAME_LIMIT];
	uint32_t length;
	uint32_t index;
	int error;

	/* Finds the property. */
	error = drv_fdt_property(fdt, node, property, &value, &length);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe tree %s none (%d)\n", label, error);
		return;
	}

	/* Copies the first string, stopping at its end, the property's end or the buffer's. */
	index = 0;
	while (index < length && index < sizeof(text) - 1U && value[index] != 0) {
		text[index] = (char)value[index];
		index++;
	}
	text[index] = 0;

	/* Logs it. */
	kern_logf("rpi5-hdmi-audio: probe tree %s \"%s\"\n", label, text);
}

/*
 * Finds one port's node and what the tree ties it to, and maps its
 * windows.  Mapping touches no register.  ENOENT means the tree has no
 * such port.
 */
static int
find_port(
	struct hdmi_audio *audio,
	uint32_t index)
{
	struct hdmi_audio_port *port;
	const uint8_t *value;
	uint32_t length;
	uint32_t provider;
	uint32_t argument;
	unsigned position;
	int error;

	port = &audio->ports[index];

	/* Finds the port's node. */
	error = drv_fdt_find_compatible(&audio->fdt, port_compatible[index], DRV_FDT_NO_NODE, &port->node);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe hdmi%u no node (%d)\n", index, error);
		return ENOENT;
	}
	port->found = true;

	/* Notes the status; the firmware's trees leave the ports disabled, so it decides nothing. */
	port->tree_enabled = drv_fdt_node_enabled(&audio->fdt, port->node);

	/* Reads the DMA cell of the port's audio request. */
	error = drv_fdt_string_index(&audio->fdt, port->node, "dma-names", "audio-rx", &position);
	if (error == 0)
		error = find_entry(&audio->fdt, port->node, "dmas", "#dma-cells", position, &provider, &argument);
	if (error == 0) {
		port->dma_found = true;
		port->dma_node = provider;
		port->dma_cell = argument;
	}

	/* Reads the reset line that holds the port in reset. */
	error = find_entry(&audio->fdt, port->node, "resets", "#reset-cells", 0, &provider, &argument);
	if (error == 0) {
		port->reset_found = true;
		port->reset_bit = argument;
	}

	/* Reads which of the clock block's gates feeds the port's audio. */
	error = drv_fdt_string_index(&audio->fdt, port->node, "clock-names", "audio", &position);
	if (error == 0)
		error = find_entry(&audio->fdt, port->node, "clocks", "#clock-cells", position, &provider, &argument);
	if (error == 0) {
		port->audio_clock_found = true;
		port->audio_clock = argument;
	}

	/*
	 * Maps the core, the shared block and the packet RAM, which the stream
	 * writes, and the PHY and the rate manager, which are only read.
	 */
	error = map_named(&audio->fdt, port->node, "hdmi", KERN_DEVICE_UNCACHED, &port->core);
	if (error == 0)
		error = map_named(&audio->fdt, port->node, "phy", KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &port->phy);
	if (error == 0)
		error = map_named(&audio->fdt, port->node, "rm", KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &port->rate);
	if (error == 0)
		error = map_named(&audio->fdt, port->node, "hd", KERN_DEVICE_UNCACHED, &port->shared);
	if (error == 0)
		error = map_named(&audio->fdt, port->node, "packet", KERN_DEVICE_UNCACHED, &port->packet);
	if (error != 0)
		kern_logf("rpi5-hdmi-audio: probe hdmi%u windows unmapped (%d)\n", index, error);

	/* Notes a node without the clock list, which the clock ids come from. */
	error = drv_fdt_property(&audio->fdt, port->node, "clocks", &value, &length);
	if (error != 0)
		kern_logf("rpi5-hdmi-audio: probe hdmi%u no clocks (%d)\n", index, error);

	/* Succeeded: the port is in the tree. */
	return 0;
}

/*
 * Finds one entry of a phandle list (clocks, resets, dmas) and its first
 * argument cell; an entry without argument cells gives argument 0.
 */
static int
find_entry(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *list,
	const char *cells,
	unsigned wanted,
	uint32_t *provider,
	uint32_t *argument)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t cell;
	uint32_t count;
	uint32_t phandle;
	uint32_t argument_cells;
	unsigned entry;
	int error;

	/* Finds the list. */
	error = drv_fdt_property(fdt, node, list, &value, &length);
	if (error != 0)
		return error;

	/* Walks the entries, each a phandle and its provider's argument cells. */
	count = length / 4U;
	cell = 0;
	entry = 0;
	while (cell < count) {
		/* Finds the entry's provider and how many arguments it takes. */
		phandle = (uint32_t)drv_fdt_cells_value(value, cell, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, provider);
		if (error != 0)
			return error;
		argument_cells = drv_fdt_node_cells(fdt, *provider, cells, 0U);

		/* Refuses an entry that runs past the list. */
		if (cell + 1U + argument_cells > count)
			return EINVAL;

		/* Takes the wanted entry's first argument. */
		if (entry == wanted) {
			*argument = 0;
			if (argument_cells != 0)
				*argument = (uint32_t)drv_fdt_cells_value(value, cell + 1U, 1U);
			return 0;
		}

		/* Moves to the next entry. */
		cell += 1U + argument_cells;
		entry++;
	}

	/* The list is shorter than the entry asked for. */
	return ENOENT;
}

/* Maps the window a node's reg-names entry names. */
static int
map_named(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	unsigned attributes,
	struct hdmi_audio_window *window)
{
	uint64_t physical;
	uint64_t size;
	unsigned position;
	int error;

	/* Finds the window by its name. */
	error = drv_fdt_string_index(fdt, node, "reg-names", name, &position);
	if (error != 0)
		return error;
	error = drv_fdt_reg(fdt, node, position, &physical, &size);
	if (error != 0)
		return error;

	/* Maps it. */
	error = map_window(physical, size, attributes, window);
	if (error != 0)
		return error;

	/* Succeeded: the window is mapped. */
	return 0;
}

/* Maps a register window as uncached device memory, read-only when asked. */
static int
map_window(
	uint64_t physical,
	uint64_t size,
	unsigned attributes,
	struct hdmi_audio_window *window)
{
	void *mapped;
	int error;

	/* Maps the range. */
	error = kern_device_map(physical, (size_t)size, attributes, &mapped);
	if (error != 0)
		return error;

	/* Records the window. */
	window->physical = physical;
	window->size = size;
	window->mapped = mapped;

	/* Succeeded: the window can be used. */
	return 0;
}

/* Reads the compositor's version, which tells the chip's stepping. */
static void
read_stepping(
	struct hdmi_audio *audio)
{
	struct hdmi_audio_window window;
	enum clock_answer core;
	enum clock_answer display;
	uint64_t physical;
	uint64_t size;
	uint32_t node;
	int error;

	/* Finds the compositor. */
	error = drv_fdt_find_compatible(&audio->fdt, "brcm,bcm2712-hvs", DRV_FDT_NO_NODE, &node);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe hvs not in the tree (%d)\n", error);
		return;
	}

	/* Leaves the compositor unread unless the firmware says both its clocks run. */
	core = firmware_clock(&audio->fdt, node, "core", "hvs-core");
	display = firmware_clock(&audio->fdt, node, "disp", "hvs-disp");
	if (core != CLOCK_ON || display != CLOCK_ON) {
		kern_logf("rpi5-hdmi-audio: probe hvs not read: clocks not known to run\n");
		return;
	}

	/* Maps the compositor's first registers. */
	error = drv_fdt_reg(&audio->fdt, node, 0, &physical, &size);
	if (error == 0)
		error = map_window(physical, HVS_WINDOW_BYTES, KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &window);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe hvs unread (%d)\n", error);
		return;
	}

	/* Reads the version and names its stepping. */
	audio->hvs_version = read_register(&window, HVS_VERSION);
	audio->stepping = drv_rpi5_hdmi_audio_stepping(audio->hvs_version);
	kern_logf("rpi5-hdmi-audio: probe hvs %llx version %08x stepping %u (1 C, 2 D0)\n",
		  (unsigned long long)physical, audio->hvs_version, (unsigned)audio->stepping);
}

/*
 * Reads the clock and reset block: the ports' resets and audio clock
 * gates.  The block is mapped writable, for the gate of the port that
 * plays.
 */
static void
read_dvp(
	struct hdmi_audio *audio)
{
	const uint8_t *value;
	uint64_t physical;
	uint64_t size;
	uint32_t length;
	uint32_t node;
	uint32_t parent;
	uint32_t phandle;
	int error;

	/* Finds and maps the block. */
	error = drv_fdt_find_compatible(&audio->fdt, "brcm,brcm2711-dvp", DRV_FDT_NO_NODE, &node);
	if (error == 0)
		error = drv_fdt_reg(&audio->fdt, node, 0, &physical, &size);
	if (error == 0)
		error = map_window(physical, DVP_WINDOW_BYTES, KERN_DEVICE_UNCACHED, &audio->dvp);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe dvp unread (%d)\n", error);
		return;
	}

	/* Reads the reset lines and the clock gates. */
	audio->dvp_reset = read_register(&audio->dvp, DVP_RESET);
	audio->dvp_gate = read_register(&audio->dvp, DVP_GATE);
	audio->dvp_read = true;

	/* Reads the rate of the clock the gates pass, the block's own parent. */
	error = drv_fdt_property(&audio->fdt, node, "clocks", &value, &length);
	if (error == 0 && length >= 4U) {
		phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
		error = drv_fdt_find_phandle(&audio->fdt, phandle, &parent);
		if (error == 0)
			error = drv_fdt_property(&audio->fdt, parent, "clock-frequency", &value, &length);
		if (error == 0 && length >= 4U)
			audio->audio_parent_hz = drv_fdt_cells_value(value, 0, 1U);
	}

	/* Logs the block. */
	kern_logf("rpi5-hdmi-audio: probe dvp %llx reset %08x gate %08x parent %llu Hz\n",
		  (unsigned long long)physical, audio->dvp_reset, audio->dvp_gate,
		  (unsigned long long)audio->audio_parent_hz);
}

/*
 * Asks the firmware whether the HDMI and BVB clocks of the first port run,
 * and keeps their numbers for the watching thread.  Both ports name the
 * same two clocks.
 */
static void
read_port_clocks(
	struct hdmi_audio *audio)
{
	uint32_t node;
	uint32_t index;
	enum clock_answer hdmi;
	enum clock_answer bvb;
	int hdmi_error;
	int bvb_error;

	/* Takes the first port the tree has. */
	node = DRV_FDT_NO_NODE;
	for (index = 0; index < HDMI_AUDIO_PORTS; index++) {
		if (node == DRV_FDT_NO_NODE && audio->ports[index].found)
			node = audio->ports[index].node;
	}
	if (node == DRV_FDT_NO_NODE)
		return;

	/* Keeps the two clocks' numbers. */
	hdmi_error = find_clock(&audio->fdt, node, "hdmi", &audio->hdmi_clock);
	bvb_error = find_clock(&audio->fdt, node, "bvb", &audio->bvb_clock);
	if (hdmi_error == 0 && bvb_error == 0)
		audio->clock_ids_found = true;

	/* Asks about both clocks. */
	hdmi = firmware_clock(&audio->fdt, node, "hdmi", "hdmi");
	bvb = firmware_clock(&audio->fdt, node, "bvb", "bvb");

	/* Records the answer: known when the firmware answered for both, on when both run. */
	if (hdmi == CLOCK_UNKNOWN || bvb == CLOCK_UNKNOWN)
		return;
	audio->clocks_known = true;
	if (hdmi == CLOCK_ON && bvb == CLOCK_ON)
		audio->clocks_on = true;
}

/*
 * Finds the firmware's number of one clock a node names.  ENOTSUP when the
 * clock is not one of the firmware's.
 */
static int
find_clock(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t *clock)
{
	uint32_t provider;
	unsigned position;
	unsigned firmware;
	int error;

	/* Finds the clock's entry and its provider. */
	error = drv_fdt_string_index(fdt, node, "clock-names", name, &position);
	if (error == 0)
		error = find_entry(fdt, node, "clocks", "#clock-cells", position, &provider, clock);
	if (error != 0)
		return error;

	/* Requires the firmware's clock, which the mailbox answers for. */
	error = drv_fdt_string_index(fdt, provider, "compatible", "raspberrypi,firmware-clocks", &firmware);
	if (error != 0)
		return ENOTSUP;

	/* Succeeded: clock holds the firmware's number. */
	return 0;
}

/*
 * Asks the firmware about one clock a node names: whether it exists and
 * runs, and its rate, and logs the answer.  For a clock that is not the
 * firmware's, or when the mailbox does not answer, the answer is unknown.
 */
static enum clock_answer
firmware_clock(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	const char *label)
{
	uint32_t clock;
	uint32_t state;
	uint32_t rate;
	int error;

	/* Finds the clock, which must be the firmware's. */
	error = find_clock(fdt, node, name, &clock);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe clock %s not a firmware clock in the tree (%d)\n", label, error);
		return CLOCK_UNKNOWN;
	}

	/* Prepares the mailbox; a call after an earlier one does nothing. */
	error = drv_rpi5_firmware_init(fdt);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe clock %s no mailbox (%d)\n", label, error);
		return CLOCK_UNKNOWN;
	}

	/* Asks for the clock's state and rate. */
	state = 0;
	rate = 0;
	error = drv_rpi5_firmware_clock_get(FIRMWARE_CLOCK_STATE, clock, &state);
	if (error == 0)
		error = drv_rpi5_firmware_clock_get(FIRMWARE_CLOCK_RATE, clock, &rate);
	kern_logf("rpi5-hdmi-audio: probe clock %s id %u state %08x rate %u (%d)\n", label, clock, state, rate, error);
	if (error != 0)
		return CLOCK_UNKNOWN;

	/* A clock that does not exist or is stopped answers no access. */
	if ((state & FIRMWARE_CLOCK_ABSENT) != 0)
		return CLOCK_OFF;
	if ((state & FIRMWARE_CLOCK_ON) == 0)
		return CLOCK_OFF;

	/* Succeeded: the clock runs. */
	return CLOCK_ON;
}

/*
 * Asks the firmware, without logging, whether one of its clocks runs.
 * Sleeps while the mailbox answers; thread context only.
 */
static enum clock_answer
ask_clock(
	const struct drv_fdt *fdt,
	uint32_t clock)
{
	uint32_t state;
	int error;

	/* Prepares the mailbox; a call after an earlier one does nothing. */
	error = drv_rpi5_firmware_init(fdt);
	if (error != 0)
		return CLOCK_UNKNOWN;

	/* Asks for the clock's state. */
	state = 0;
	error = drv_rpi5_firmware_clock_get(FIRMWARE_CLOCK_STATE, clock, &state);
	if (error != 0)
		return CLOCK_UNKNOWN;

	/* A clock that does not exist or is stopped answers no access. */
	if ((state & FIRMWARE_CLOCK_ABSENT) != 0)
		return CLOCK_OFF;
	if ((state & FIRMWARE_CLOCK_ON) == 0)
		return CLOCK_OFF;

	/* Succeeded: the clock runs. */
	return CLOCK_ON;
}

/* Gives why a port must not be read, or NULL when it may be. */
static const char *
port_unreadable(
	const struct hdmi_audio *audio,
	const struct hdmi_audio_port *port)
{
	/* Needs the reset state and the port's reset line. */
	if (!audio->dvp_read)
		return "clock and reset block unread";
	if (!port->reset_found)
		return "no reset line in the tree";

	/* A port held in reset does not answer. */
	if (port->reset_bit >= 32U)
		return "reset line out of range";
	if ((audio->dvp_reset & (1U << port->reset_bit)) != 0)
		return "held in reset";

	/* A port whose clocks are stopped, or may be, does not answer. */
	if (!audio->clocks_known)
		return "firmware clock state unknown";
	if (!audio->clocks_on)
		return "firmware clocks off";

	/* Needs every window mapped. */
	if (port->core.mapped == NULL || port->phy.mapped == NULL)
		return "windows unmapped";
	if (port->rate.mapped == NULL || port->shared.mapped == NULL)
		return "windows unmapped";

	/* The port may be read. */
	return NULL;
}

/* Reads and logs one port's audio, packet, timing and PHY registers. */
static void
read_port(
	struct hdmi_audio *audio,
	uint32_t index)
{
	struct hdmi_audio_port *port;
	const char *reason;
	uint32_t words[PORT_LOG_WORDS];
	uint32_t fifo;
	uint32_t dreq;
	uint32_t offset;
	uint32_t reference;
	uint32_t divider;
	bool agrees;
	int serror;

	port = &audio->ports[index];

	/* Leaves a port the tree does not have. */
	if (!port->found)
		return;

	/* Logs what the tree ties the port to, and the DREQ the stepping gives. */
	agrees = drv_rpi5_hdmi_audio_dreq(audio->stepping, index, port->dma_cell, &dreq);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u core %llx enabled %u dma-found %u cell %08x dreq %u agrees %u reset-found %u reset-bit %u audio-found %u audio-clock %u\n",
		  index, (unsigned long long)port->core.physical, port->tree_enabled ? 1U : 0U,
		  port->dma_found ? 1U : 0U, port->dma_cell, dreq, agrees ? 1U : 0U,
		  port->reset_found ? 1U : 0U, port->reset_bit,
		  port->audio_clock_found ? 1U : 0U, port->audio_clock);

	/* Leaves a port that may not answer. */
	reason = port_unreadable(audio, port);
	if (reason != NULL) {
		kern_logf("rpi5-hdmi-audio: probe hdmi%u not read: %s\n", index, reason);
		return;
	}

	/* Reads the registers the choice of the port depends on. */
	port->scheduler = read_register(&port->core, CORE_SCHEDULER);
	port->hotplug = read_register(&port->core, CORE_HOTPLUG);
	port->read = true;

	/* Reads and logs the core's audio and packet registers. */
	words[0] = read_register(&port->core, CORE_PACKET_CONFIG);
	words[1] = read_register(&port->core, CORE_PACKET_STATUS);
	words[2] = read_register(&port->core, CORE_CLOCK_REGEN);
	words[3] = read_register(&port->core, CORE_CTS_0);
	words[4] = read_register(&port->core, CORE_CTS_1);
	words[5] = read_register(&port->core, CORE_AUDIO_PACKET);
	words[6] = read_register(&port->core, CORE_AUDIO_CONFIG);
	words[7] = read_register(&port->core, CORE_CHANNEL_MAP);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u audio scheduler %08x hotplug %08x packet-config %08x packet-status %08x regen %08x cts0 %08x cts1 %08x audio-packet %08x audio-config %08x channel-map %08x\n",
		  index, port->scheduler, port->hotplug, words[0], words[1], words[2],
		  words[3], words[4], words[5], words[6], words[7]);

	/* Reads and logs the core's timing, which gives the mode the firmware set. */
	words[0] = read_register(&port->core, CORE_HORIZONTAL_A);
	words[1] = read_register(&port->core, CORE_HORIZONTAL_B);
	words[2] = read_register(&port->core, CORE_VERTICAL_A0);
	words[3] = read_register(&port->core, CORE_VERTICAL_B0);
	words[4] = read_register(&port->core, CORE_VERTICAL_A1);
	words[5] = read_register(&port->core, CORE_VERTICAL_B1);
	words[6] = read_register(&port->core, CORE_MISC);
	words[7] = read_register(&port->core, CORE_DEEP_COLOUR);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u timing ha %08x hb %08x va0 %08x vb0 %08x va1 %08x vb1 %08x misc %08x deep %08x\n",
		  index, words[0], words[1], words[2], words[3], words[4], words[5],
		  words[6], words[7]);

	/* Reads and logs the port's audio FIFO registers. */
	fifo = port_fifo[index];
	words[0] = read_register(&port->shared, fifo + FIFO_CONTROL);
	words[1] = read_register(&port->shared, fifo + FIFO_THRESHOLD);
	words[2] = read_register(&port->shared, fifo + FIFO_FORMAT);
	words[3] = read_register(&port->shared, fifo + FIFO_SAMPLE_RATE);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u fifo control %08x threshold %08x format %08x sample-rate %08x\n",
		  index, words[0], words[1], words[2], words[3]);

	/* Reads and logs the PHY and the rate manager, which give the TMDS rate. */
	words[0] = read_register(&port->phy, PHY_RESET);
	words[1] = read_register(&port->phy, PHY_POWER);
	reference = read_register(&port->phy, PHY_REFERENCE);
	divider = read_register(&port->phy, PHY_VCO_DIVIDER);
	words[2] = read_register(&port->rate, RATE_CONTROL);
	offset = read_register(&port->rate, RATE_OFFSET);
	words[3] = read_register(&port->rate, RATE_FORMAT);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u phy reset %08x power %08x reference %08x vco-divider %08x rate-control %08x rate-offset %08x rate-format %08x\n",
		  index, words[0], words[1], reference, divider, words[2], offset, words[3]);
	report_rate(audio, index, offset, reference, divider);

	/* Checks that the reads left no SError, and logs the answer. */
	serror = drv_rpi5_serror_check(port_step[index]);
	kern_logf("rpi5-hdmi-audio: probe hdmi%u read, serror %d\n", index, serror);
}

/* Logs the TMDS rate the PHY gives, and the clock regeneration and sample divider it leads to. */
static void
report_rate(
	const struct hdmi_audio *audio,
	uint32_t index,
	uint32_t offset,
	uint32_t reference,
	uint32_t divider)
{
	struct drv_rpi5_hdmi_audio_clock hardware;
	struct drv_rpi5_hdmi_audio_clock software;
	enum drv_rpi5_hdmi_audio_tmds_status status;
	uint64_t tmds_hz;
	uint32_t periods;
	uint32_t samples;
	uint32_t word;

	/* Works the TMDS rate out of the readback. */
	tmds_hz = 0;
	status = drv_rpi5_hdmi_audio_tmds_rate(offset, reference, divider, &tmds_hz);
	if (status != DRV_RPI5_HDMI_AUDIO_TMDS_OK) {
		kern_logf("rpi5-hdmi-audio: probe hdmi%u tmds unknown (status %u)\n", index, (unsigned)status);
		return;
	}

	/* The N and CTS of both ways of regenerating the clock. */
	drv_rpi5_hdmi_audio_clock(tmds_hz, true, &hardware);
	drv_rpi5_hdmi_audio_clock(tmds_hz, false, &software);

	/* The sample divider of the audio clock. */
	word = 0;
	if (audio->audio_parent_hz != 0 && audio->audio_parent_hz <= 0xffffffffULL) {
		drv_rpi5_hdmi_audio_sample_divider((uint32_t)audio->audio_parent_hz, DRV_RPI5_HDMI_AUDIO_RATE, &periods, &samples);
		word = drv_rpi5_hdmi_audio_sample_word(periods, samples);
	}

	/* Logs them. */
	kern_logf("rpi5-hdmi-audio: probe hdmi%u tmds %llu Hz n %u cts %u (hardware) n %u cts %u exact %u (software) sample-word %08x\n",
		  index, (unsigned long long)tmds_hz, hardware.n, hardware.cts,
		  software.n, software.cts, software.exact ? 1U : 0U, word);
}

/*
 * Reads each channel of the DMA controller the ports name: its status,
 * control block, debug and transfer words.
 */
static void
read_dma(
	const struct hdmi_audio *audio)
{
	struct hdmi_audio_window window;
	const uint8_t *value;
	uint64_t physical;
	uint64_t size;
	uint32_t length;
	uint32_t node;
	uint32_t mask;
	uint32_t first;
	uint32_t count;
	uint32_t channel;
	uint32_t base;
	uint32_t status;
	uint32_t block;
	uint32_t debug;
	uint32_t transfer;
	uint32_t index;
	int error;

	/*
	 * Finds the controller the first port's audio request names; the
	 * tree has other controllers of the same kind.
	 */
	node = DRV_FDT_NO_NODE;
	for (index = 0; index < HDMI_AUDIO_PORTS; index++) {
		if (node == DRV_FDT_NO_NODE && audio->ports[index].dma_found)
			node = audio->ports[index].dma_node;
	}
	if (node == DRV_FDT_NO_NODE) {
		kern_logf("rpi5-hdmi-audio: probe dma none named\n");
		return;
	}

	/* Maps the channels for reading. */
	error = drv_fdt_reg(&audio->fdt, node, 0, &physical, &size);
	if (error == 0)
		error = map_window(physical, size, KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &window);
	if (error != 0) {
		kern_logf("rpi5-hdmi-audio: probe dma unread (%d)\n", error);
		return;
	}

	/* Reads the channels the tree lets the kernel use. */
	mask = 0;
	error = drv_fdt_property(&audio->fdt, node, "brcm,dma-channel-mask", &value, &length);
	if (error == 0 && length >= 4U)
		mask = (uint32_t)drv_fdt_cells_value(value, 0, 1U);

	/* Logs every channel of the window. */
	first = (uint32_t)(physical >> DMA_CHANNEL_SHIFT) & DMA_CHANNEL_MASK;
	count = (uint32_t)(size / DMA_CHANNEL_STRIDE);
	kern_logf("rpi5-hdmi-audio: probe dma %llx first %u count %u mask %08x\n",
		  (unsigned long long)physical, first, count, mask);
	for (channel = 0; channel < count && first + channel < DMA_CHANNEL_LIMIT; channel++) {
		base = channel * DMA_CHANNEL_STRIDE;
		status = read_register(&window, base + DMA_STATUS);
		block = read_register(&window, base + DMA_BLOCK);
		debug = read_register(&window, base + DMA_DEBUG);
		transfer = read_register(&window, base + DMA_TRANSFER);
		kern_logf("rpi5-hdmi-audio: probe dma channel %u cs %08x cb %08x debug %08x ti %08x\n",
			  first + channel, status, block, debug, transfer);
	}
}

/*
 * Names the first port, HDMI0 then HDMI1, that is out of reset, sends
 * HDMI rather than DVI, and sees a sink.  ENODEV when none is.
 */
static int
choose_port(
	const struct hdmi_audio *audio,
	uint32_t *chosen)
{
	const struct hdmi_audio_port *port;
	uint32_t index;
	uint32_t dreq;
	bool agrees;

	/* Looks at the ports in order. */
	for (index = 0; index < HDMI_AUDIO_PORTS; index++) {
		port = &audio->ports[index];

		/* Skips a port that was not read, sends DVI, or sees no sink. */
		if (!port->read)
			continue;
		if ((port->scheduler & SCHEDULER_HDMI) == 0)
			continue;
		if ((port->hotplug & HOTPLUG_CONNECTED) == 0)
			continue;

		/* Names the port and the DREQ the stepping gives it. */
		agrees = drv_rpi5_hdmi_audio_dreq(audio->stepping, index, port->dma_cell, &dreq);
		kern_logf("rpi5-hdmi-audio: probe choice hdmi%u dreq %u (tree agrees %u)\n",
			  index, dreq, agrees ? 1U : 0U);
		*chosen = index;
		return 0;
	}

	/* No port could carry audio. */
	kern_logf("rpi5-hdmi-audio: probe choice none\n");
	kern_logf("rpi5-hdmi-audio: no HDMI port takes sound; no audio device\n");
	return ENODEV;
}

/*
 * Sets up the stream of the chosen port -- its DMA channel and interrupt,
 * the subframe ring, the watching thread -- and offers it to the audio
 * core.  A failure leaves what was mapped and allocated, as a permanent
 * board device would, but offers nothing.
 */
static int
attach_stream(
	struct hdmi_audio *audio,
	uint32_t index)
{
	struct hdmi_audio_port *port;
	struct thread *thread;
	unsigned long enabled;
	int error;

	port = &audio->ports[index];
	audio->port = index;

	/* Works out what the port and the DMA need, and the subframe ring. */
	error = prepare_stream_hardware(audio, port);
	if (error != 0)
		return error;

	/* The locks, and the queue the watching thread sleeps on. */
	spin_init(&audio->lock, LOCK_RANK_DEVICE, "rpi5 hdmi audio");
	error = mutex_init(&audio->control, LOCK_RANK_DEVICE, "rpi5 hdmi audio control");
	if (error != 0)
		return error;
	waitq_init(&audio->wake, "rpi5 hdmi audio watch");

	/* Leaves the channel idle before its interrupt is bound. */
	enabled = spin_lock_irqsave(&audio->lock);

	halt_dma_locked(audio);

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Binds the channel's interrupt. */
	error = start_service(audio);
	if (error != 0)
		return error;

	/* Creates the watching thread, which sleeps until a stream runs. */
	error = kthread_create(watch_thread, audio, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_irq_mask(audio->irq);
		return error;
	}
	audio->thread = thread;

	/* Offers the port to the audio core. */
	audio->ops.playback = 1U;
	audio->ops.capture = 0U;
	audio->ops.formats = hdmi_audio_formats;
	audio->ops.format_count = sizeof(hdmi_audio_formats) / sizeof(hdmi_audio_formats[0]);
	audio->ops.prepare = audio_prepare;
	audio->ops.start = audio_start;
	audio->ops.stop = audio_stop;
	audio->ops.position = audio_position;
	error = drv_audio_register(&audio->ops, audio, audio->dma_device, &audio->device);
	if (error != 0) {
		(void)thread_abort_new(thread);
		kern_irq_mask(audio->irq);
		return error;
	}

	/* Lets the thread run. */
	thread_start(thread);

	/* Succeeded: one line says what plays. */
	kern_logf("rpi5-hdmi-audio: hdmi%u stepping %u dreq %u (tree agrees %u) dma channel %u irq %d tmds %llu Hz; 48 kHz 16-bit stereo\n",
		  index, (unsigned)audio->stepping, audio->request.dreq, audio->dreq_agrees ? 1U : 0U,
		  audio->channel, audio->irq, (unsigned long long)audio->written.tmds_hz);
	return 0;
}

/*
 * Works out the chosen port's DMA request, FIFO thresholds and sample
 * divider, finds a DMA channel, checks that the DMA reaches the FIFO and
 * the driver's buffer at their own addresses, and builds the ring of
 * control blocks.
 */
static int
prepare_stream_hardware(
	struct hdmi_audio *audio,
	struct hdmi_audio_port *port)
{
	struct drv_dma_constraints constraints;
	uint64_t tmds_hz;
	uint32_t periods;
	uint32_t samples;
	uint32_t dreq;
	bool known;
	bool reaches;
	bool built;
	int error;

	/* Needs the stepping: it decides the DREQ and the threshold layout. */
	if (audio->stepping == DRV_RPI5_HDMI_AUDIO_STEPPING_UNKNOWN) {
		kern_logf("rpi5-hdmi-audio: hdmi%u stepping unknown\n", audio->port);
		return ENODEV;
	}

	/* The port's DMA request: the cell's shape and priorities, the stepping's DREQ. */
	if (!port->dma_found)
		return ENODEV;
	drv_rpi5_hdmi_audio_dma_decode(port->dma_cell, &audio->request);
	audio->dreq_agrees = drv_rpi5_hdmi_audio_dreq(audio->stepping, audio->port, port->dma_cell, &dreq);
	audio->request.dreq = dreq;
	audio->run_word = drv_rpi5_hdmi_audio_dma_run(&audio->request);

	/* The FIFO thresholds of the stepping. */
	known = drv_rpi5_hdmi_audio_thresholds(audio->stepping, &audio->threshold_word);
	if (!known)
		return ENODEV;

	/* The sample divider of the audio clock. */
	if (audio->audio_parent_hz == 0 || audio->audio_parent_hz > 0xffffffffULL)
		return ENODEV;
	drv_rpi5_hdmi_audio_sample_divider((uint32_t)audio->audio_parent_hz, DRV_RPI5_HDMI_AUDIO_RATE, &periods, &samples);
	audio->sample_word = drv_rpi5_hdmi_audio_sample_word(periods, samples);

	/* The gate of the port's audio clock, and the firmware clocks the thread asks about. */
	if (!port->audio_clock_found || port->audio_clock >= DVP_GATE_COUNT)
		return ENODEV;
	if (!audio->clock_ids_found || port->packet.mapped == NULL)
		return ENODEV;

	/* A TMDS rate the PHY's readback gives. */
	known = read_tmds(audio, &tmds_hz);
	if (!known) {
		kern_logf("rpi5-hdmi-audio: hdmi%u TMDS rate unknown\n", audio->port);
		return ENODEV;
	}
	audio->written.tmds_hz = tmds_hz;

	/* A DMA channel and its interrupt. */
	error = choose_channel(audio, port->dma_node);
	if (error != 0)
		return error;

	/* The FIFO the DMA writes, at an address its bus maps to itself. */
	audio->fifo_bus = port->shared.physical + port_fifo[audio->port] + FIFO_DATA;
	reaches = bus_identity(&audio->fdt, port->dma_node, audio->fifo_bus, 4U);
	if (!reaches) {
		kern_logf("rpi5-hdmi-audio: the DMA does not reach the FIFO at %llx\n", (unsigned long long)audio->fifo_bus);
		return ENODEV;
	}

	/* The DMA's view of memory: 40 bits, the same addresses, uncached. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = WIRE_ADDRESS_BITS;
	constraints.max_segment_size = WIRE_SEGMENT_BYTES;
	constraints.segment_boundary = WIRE_SEGMENT_BOUNDARY;
	constraints.coherent = 0;
	error = drv_dma_device_create(&constraints, &audio->dma_device);
	if (error != 0)
		return error;

	/* The control blocks and the subframe ring, which the DMA reads at their own addresses. */
	error = drv_dma_alloc_coherent(audio->dma_device, WIRE_CB_AREA + WIRE_RING_BYTES, WIRE_ALIGNMENT, &audio->wire);
	if (error != 0)
		return error;
	reaches = bus_identity(&audio->fdt, port->dma_node, audio->wire.device_address, audio->wire.size);
	if (!reaches) {
		kern_logf("rpi5-hdmi-audio: the DMA does not reach its ring at %llx\n", (unsigned long long)audio->wire.device_address);
		return ENODEV;
	}
	audio->cbs = audio->wire.address;
	audio->words = (uint32_t *)((uint8_t *)audio->wire.address + WIRE_CB_AREA);
	audio->cb_bus = audio->wire.device_address;
	audio->data_bus = audio->cb_bus + WIRE_CB_AREA;
	kern_memset(audio->wire.address, 0, WIRE_CB_AREA + WIRE_RING_BYTES);

	/* The ring of control blocks, one per fragment, the last back to the first. */
	built = drv_rpi5_hdmi_audio_dma_ring(audio->cbs, DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT, audio->cb_bus, audio->data_bus,
					     DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES, audio->fifo_bus, &audio->request);
	if (!built)
		return EINVAL;

	/* The channel status block every stream sends. */
	drv_rpi5_hdmi_audio_channel_status(audio->status);

	/* Succeeded: the stream's hardware is known. */
	return 0;
}

/*
 * Chooses the DMA channel: the highest one the tree leaves to the kernel
 * that is a 40-bit channel and idle, and finds its interrupt.  The
 * channel's registers are mapped writable.
 */
static int
choose_channel(
	struct hdmi_audio *audio,
	uint32_t node)
{
	const uint8_t *value;
	uint64_t physical;
	uint64_t size;
	uint32_t length;
	uint32_t mask;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint32_t channel;
	uint32_t block;
	int error;

	/* The controller's channels and the ones the kernel may use. */
	error = drv_fdt_reg(&audio->fdt, node, 0, &physical, &size);
	if (error != 0)
		return error;
	error = drv_fdt_property(&audio->fdt, node, "brcm,dma-channel-mask", &value, &length);
	if (error != 0)
		return error;
	if (length < 4U)
		return EINVAL;
	mask = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	first = (uint32_t)(physical >> DMA_CHANNEL_SHIFT) & DMA_CHANNEL_MASK;
	count = (uint32_t)(size / DMA_CHANNEL_STRIDE);

	/* Looks at the channels from the highest down for a free, idle 40-bit one. */
	for (index = count; index > 0U; index--) {
		/* Skips a channel the kernel may not use or that is not a 40-bit one. */
		channel = first + index - 1U;
		if (channel >= DMA_CHANNEL_LIMIT || channel < DMA_WIDE_FIRST)
			continue;
		if ((mask & (1U << channel)) == 0)
			continue;

		/* Maps the channel's registers. */
		error = map_window(physical + (uint64_t)(index - 1U) * DMA_CHANNEL_STRIDE, DMA_CHANNEL_STRIDE,
				   KERN_DEVICE_UNCACHED, &audio->channel_window);
		if (error != 0)
			return error;

		/* Skips a channel that holds a control block: someone else runs it. */
		block = read_register(&audio->channel_window, DMA_BLOCK);
		if (block != 0)
			continue;

		/* Takes this channel and its interrupt. */
		audio->channel = channel;
		error = find_channel_interrupt(audio, node, channel);
		if (error != 0)
			return error;
		return 0;
	}

	/* No channel is free. */
	kern_logf("rpi5-hdmi-audio: no free DMA channel (mask %08x)\n", mask);
	return EBUSY;
}

/* Finds the shared interrupt line of one DMA channel, named "dmaN" in the tree. */
static int
find_channel_interrupt(
	struct hdmi_audio *audio,
	uint32_t node,
	uint32_t channel)
{
	const uint8_t *value;
	char name[8];
	uint32_t length;
	uint64_t kind;
	uint64_t line;
	unsigned position;
	int error;

	/* The channel's interrupt name: "dma" and its number. */
	name[0] = 'd';
	name[1] = 'm';
	name[2] = 'a';
	if (channel >= 10U) {
		name[3] = (char)('0' + channel / 10U);
		name[4] = (char)('0' + channel % 10U);
		name[5] = 0;
	} else {
		name[3] = (char)('0' + channel);
		name[4] = 0;
	}

	/* Finds its specifier: a shared line of the interrupt controller. */
	error = drv_fdt_string_index(&audio->fdt, node, "interrupt-names", name, &position);
	if (error != 0)
		return error;
	error = drv_fdt_property(&audio->fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if ((uint64_t)(position + 1U) * GIC_CELLS * 4U > length)
		return EINVAL;
	kind = drv_fdt_cells_value(value, position * GIC_CELLS, 1U);
	if (kind != GIC_KIND_SPI)
		return ENOTSUP;
	line = drv_fdt_cells_value(value, position * GIC_CELLS + 1U, 1U);
	if (line >= GIC_SPI_LIMIT)
		return ERANGE;

	/* Succeeded: the line's number after the private ones. */
	audio->irq = (int)(line + GIC_SPI_BASE);
	return 0;
}

/*
 * Tells whether the bus a DMA controller sits on maps a range of addresses
 * to the same addresses, by its parent's dma-ranges.  A bus without
 * dma-ranges is not taken to map anything.
 *
 * Value referenced: raspberrypi/firmware 3d301dd boot/bcm2712-rpi-5-b.dtb
 * (the axi bus's identity dma-ranges).
 */
static bool
bus_identity(
	const struct drv_fdt *fdt,
	uint32_t node,
	uint64_t address,
	uint64_t bytes)
{
	const uint8_t *value;
	uint64_t child;
	uint64_t parent_address;
	uint64_t size;
	uint32_t length;
	uint32_t bus;
	uint32_t above;
	uint32_t child_cells;
	uint32_t parent_cells;
	uint32_t size_cells;
	uint32_t entry_cells;
	uint32_t cell;
	int error;

	/* The bus and its own parent, whose cell counts shape the entries. */
	error = drv_fdt_parent(fdt, node, &bus);
	if (error != 0)
		return false;
	child_cells = drv_fdt_node_cells(fdt, bus, "#address-cells", 2U);
	size_cells = drv_fdt_node_cells(fdt, bus, "#size-cells", 1U);
	parent_cells = 2U;
	error = drv_fdt_parent(fdt, bus, &above);
	if (error == 0)
		parent_cells = drv_fdt_node_cells(fdt, above, "#address-cells", 2U);
	if (child_cells == 0U || child_cells > 2U || parent_cells == 0U || parent_cells > 2U)
		return false;
	if (size_cells == 0U || size_cells > 2U)
		return false;

	/* The bus's DMA ranges. */
	error = drv_fdt_property(fdt, bus, "dma-ranges", &value, &length);
	if (error != 0)
		return false;

	/* Looks through the entries for one that maps the range to itself. */
	entry_cells = child_cells + parent_cells + size_cells;
	for (cell = 0U; (cell + entry_cells) * 4U <= length; cell += entry_cells) {
		/* The entry's child address, parent address and size. */
		child = drv_fdt_cells_value(value, cell, child_cells);
		parent_address = drv_fdt_cells_value(value, cell + child_cells, parent_cells);
		size = drv_fdt_cells_value(value, cell + child_cells + parent_cells, size_cells);

		/* Skips an entry that moves addresses or does not hold the whole range. */
		if (child != parent_address)
			continue;
		if (address < child || bytes > size)
			continue;
		if (address - child > size - bytes)
			continue;

		/* The range keeps its own addresses. */
		return true;
	}

	/* No entry maps the range to itself. */
	return false;
}

/* Binds the DMA channel's interrupt, a level-high shared line. */
static int
start_service(
	struct hdmi_audio *audio)
{
	int error;

	/* Masks the line while it is set up. */
	kern_irq_mask(audio->irq);
	error = kern_irq_set_mode(audio->irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0)
		return error;

	/* Hands it to the driver's handler, which ignores an idle channel. */
	error = kern_irq_register(audio->irq, audio_interrupt, audio);
	if (error != 0)
		return error;
	kern_irq_unmask(audio->irq);

	/* Succeeded: the channel's interrupts reach the driver. */
	return 0;
}

/*
 * Works the chosen port's TMDS rate out of its PHY.  The port must be
 * readable.  False when the readback gives no rate.
 */
static bool
read_tmds(
	const struct hdmi_audio *audio,
	uint64_t *tmds_hz)
{
	const struct hdmi_audio_port *port;
	enum drv_rpi5_hdmi_audio_tmds_status status;
	uint32_t reference;
	uint32_t divider;
	uint32_t offset;

	port = &audio->ports[audio->port];

	/* Reads the reference, the VCO divider and the rate manager's offset. */
	reference = read_register(&port->phy, PHY_REFERENCE);
	divider = read_register(&port->phy, PHY_VCO_DIVIDER);
	offset = read_register(&port->rate, RATE_OFFSET);

	/* Works the rate out. */
	*tmds_hz = 0;
	status = drv_rpi5_hdmi_audio_tmds_rate(offset, reference, divider, tmds_hz);
	if (status != DRV_RPI5_HDMI_AUDIO_TMDS_OK)
		return false;

	/* Succeeded: the rate is known. */
	return true;
}

/*
 * Tells whether the chosen port takes sound: out of reset, its clocks
 * running by the firmware's account, sending HDMI, a sink on it.  The
 * port's own registers are read only after the first two.  The firmware
 * is asked when ask_now says so or its last answer is older than
 * WATCH_CLOCK_MS; otherwise the last answer stands.  Control mutex held;
 * may sleep.
 */
static bool
port_alive(
	struct hdmi_audio *audio,
	bool ask_now)
{
	const struct hdmi_audio_port *port;
	enum clock_answer hdmi;
	enum clock_answer bvb;
	uint64_t now;
	uint64_t period;
	uint32_t reset;
	uint32_t scheduler;
	uint32_t hotplug;
	bool ask;

	port = &audio->ports[audio->port];

	/* A port held in reset does not answer. */
	reset = read_register(&audio->dvp, DVP_RESET);
	if ((reset & (1U << port->reset_bit)) != 0)
		return false;

	/* Asks the firmware again when told to, or when the last answer is too old. */
	now = sched_ticks();
	period = kern_ms_to_ticks(WATCH_CLOCK_MS);
	ask = ask_now;
	if (!audio->clock_asked || now - audio->clock_tick >= period)
		ask = true;
	if (ask) {
		hdmi = ask_clock(&audio->fdt, audio->hdmi_clock);
		bvb = ask_clock(&audio->fdt, audio->bvb_clock);
		audio->clock_alive = false;
		if (hdmi == CLOCK_ON && bvb == CLOCK_ON)
			audio->clock_alive = true;
		audio->clock_asked = true;
		audio->clock_tick = now;
	}

	/* A port whose clocks are stopped does not answer. */
	if (!audio->clock_alive)
		return false;

	/* A port that sends DVI carries no sound. */
	scheduler = read_register(&port->core, CORE_SCHEDULER);
	if ((scheduler & SCHEDULER_HDMI) == 0)
		return false;

	/* A port without a sink has nobody to hear it. */
	hotplug = read_register(&port->core, CORE_HOTPLUG);
	if ((hotplug & HOTPLUG_CONNECTED) == 0)
		return false;

	/* The port takes sound. */
	return true;
}

/*
 * Sets the chosen port up for 16-bit stereo at 48 kHz: opens the audio
 * clock's gate, resets the FIFO and sets its divider, control, format and
 * thresholds, sets the port's audio configuration, channel map, packet
 * configuration and clock regeneration, and sends the audio InfoFrame.
 * Then records what the port holds.  The port must take sound.  False
 * when the PHY gives no TMDS rate.  Control mutex held; may sleep.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (the order of vc4_hdmi_audio_startup,
 * vc4_hdmi_audio_prepare and vc4_hdmi_set_n_cts).
 */
static bool
program_port(
	struct hdmi_audio *audio)
{
	struct drv_rpi5_hdmi_audio_clock regen;
	struct hdmi_audio_port *port;
	struct hdmi_audio_written *written;
	unsigned long enabled;
	uint64_t tmds_hz;
	uint32_t fifo;
	uint32_t gate;
	uint32_t regen_word;
	bool known;

	port = &audio->ports[audio->port];
	written = &audio->written;
	fifo = port_fifo[audio->port];

	/* The clock regeneration of the TMDS rate the firmware set. */
	known = read_tmds(audio, &tmds_hz);
	if (!known)
		return false;
	drv_rpi5_hdmi_audio_clock(tmds_hz, HDMI_AUDIO_HARDWARE_CTS, &regen);
	regen_word = drv_rpi5_hdmi_audio_regen_word(&regen, HDMI_AUDIO_HARDWARE_CTS);

	/* Writes the gate, the FIFO and the port's audio registers under the lock. */
	enabled = spin_lock_irqsave(&audio->lock);

	gate = read_register(&audio->dvp, DVP_GATE);
	gate &= ~(1U << (DVP_GATE_FIRST_BIT + port->audio_clock));
	write_register(&audio->dvp, DVP_GATE, gate);
	write_register(&port->shared, fifo + FIFO_CONTROL,
		       DRV_RPI5_HDMI_AUDIO_FIFO_RESET | DRV_RPI5_HDMI_AUDIO_FIFO_FLUSH | DRV_RPI5_HDMI_AUDIO_FIFO_LATE |
			       DRV_RPI5_HDMI_AUDIO_FIFO_UNDERFLOW | DRV_RPI5_HDMI_AUDIO_FIFO_OVERFLOW);
	write_register(&port->shared, fifo + FIFO_SAMPLE_RATE, audio->sample_word);
	write_register(&port->shared, fifo + FIFO_CONTROL, drv_rpi5_hdmi_audio_fifo_control(DRV_RPI5_HDMI_AUDIO_CHANNELS));
	write_register(&port->shared, fifo + FIFO_FORMAT, drv_rpi5_hdmi_audio_fifo_format());
	write_register(&port->shared, fifo + FIFO_THRESHOLD, audio->threshold_word);
	write_register(&port->core, CORE_AUDIO_CONFIG, drv_rpi5_hdmi_audio_stream_config(DRV_RPI5_HDMI_AUDIO_CHANNELS));
	write_register(&port->core, CORE_CHANNEL_MAP, drv_rpi5_hdmi_audio_channel_map(DRV_RPI5_HDMI_AUDIO_CHANNELS));
	write_register(&port->core, CORE_AUDIO_PACKET, drv_rpi5_hdmi_audio_packet_config(DRV_RPI5_HDMI_AUDIO_CHANNELS));
	write_register(&port->core, CORE_CLOCK_REGEN, regen_word);
	write_register(&port->core, CORE_CTS_0, regen.cts);
	write_register(&port->core, CORE_CTS_1, regen.cts);

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Sends the audio InfoFrame. */
	write_infoframe(audio);

	/* Records what the port holds now, as the port reads it back. */
	written->tmds_hz = tmds_hz;
	written->regen = read_register(&port->core, CORE_CLOCK_REGEN);
	written->cts = read_register(&port->core, CORE_CTS_0);
	written->sample = read_register(&port->shared, fifo + FIFO_SAMPLE_RATE);
	written->format = read_register(&port->shared, fifo + FIFO_FORMAT);
	written->threshold = read_register(&port->shared, fifo + FIFO_THRESHOLD);
	audio->programmed = true;

	/* Logs the set-up when the rate is new. */
	if (tmds_hz != audio->logged_tmds_hz) {
		audio->logged_tmds_hz = tmds_hz;
		kern_logf("rpi5-hdmi-audio: hdmi%u set up: tmds %llu Hz n %u cts %u (%s), infoframe %s\n",
			  audio->port, (unsigned long long)tmds_hz, regen.n, regen.cts,
			  HDMI_AUDIO_HARDWARE_CTS ? "port measures" : "driver's", written->infoframe ? "sent" : "not sent");
	}

	/* Succeeded: the port takes the stream. */
	return true;
}

/*
 * Writes the audio InfoFrame into its packet RAM slot: takes the slot out
 * of service, writes the frame and the zeros after it, and puts it back.
 * A packet RAM the firmware left off holds no packet, so nothing is sent.
 * Control mutex held; may sleep.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (vc4_hdmi_stop_packet and
 * vc4_hdmi_write_infoframe).
 */
static void
write_infoframe(
	struct hdmi_audio *audio)
{
	struct hdmi_audio_port *port;
	uint8_t frame[DRV_RPI5_HDMI_AUDIO_INFOFRAME_BYTES];
	uint32_t words[DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS];
	unsigned long enabled;
	uint32_t offset;
	uint32_t slot_bit;
	uint32_t config;
	uint32_t index;
	bool settled;

	port = &audio->ports[audio->port];
	audio->written.infoframe = false;

	/* The slot's place and its bit. */
	offset = drv_rpi5_hdmi_audio_packet_slot(DRV_RPI5_HDMI_AUDIO_INFOFRAME_TYPE);
	slot_bit = 1U << (offset / DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_BYTES);

	/* Sends nothing when the packet RAM is off. */
	config = read_register(&port->core, CORE_PACKET_CONFIG);
	if ((config & PACKET_CONFIG_ENABLE) == 0)
		return;

	/* Takes the slot out of service and waits until the port no longer sends it. */
	enabled = spin_lock_irqsave(&audio->lock);

	config = read_register(&port->core, CORE_PACKET_CONFIG);
	write_register(&port->core, CORE_PACKET_CONFIG, config & ~slot_bit);

	spin_unlock_irqrestore(&audio->lock, enabled);
	settled = wait_packet(port, slot_bit, false);
	if (!settled)
		kern_logf("rpi5-hdmi-audio: hdmi%u audio InfoFrame slot stayed busy\n", audio->port);

	/* Builds the frame and lays it out as the slot's words. */
	drv_rpi5_hdmi_audio_infoframe(frame, DRV_RPI5_HDMI_AUDIO_CHANNELS);
	drv_rpi5_hdmi_audio_packet_words(frame, words);

	/* Writes the slot and puts it back into service. */
	enabled = spin_lock_irqsave(&audio->lock);

	for (index = 0U; index < DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_WORDS; index++)
		write_register(&port->packet, offset + index * 4U, words[index]);
	config = read_register(&port->core, CORE_PACKET_CONFIG);
	write_register(&port->core, CORE_PACKET_CONFIG, config | slot_bit);

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Waits until the port sends it. */
	settled = wait_packet(port, slot_bit, true);
	if (!settled)
		kern_logf("rpi5-hdmi-audio: hdmi%u audio InfoFrame not taken up\n", audio->port);
	audio->written.infoframe = true;
}

/*
 * Waits up to 100 ms until the packet RAM's status shows a slot busy (or
 * idle).  False when it did not.
 */
static bool
wait_packet(
	const struct hdmi_audio_port *port,
	uint32_t slot_bit,
	bool busy)
{
	uint32_t status;
	unsigned polls;

	/* Looks at the status until it shows what is waited for. */
	for (polls = 0U; polls < PACKET_WAIT_POLLS; polls++) {
		/* Stops once the slot is as wanted. */
		status = read_register(&port->core, CORE_PACKET_STATUS);
		if (busy && (status & slot_bit) != 0)
			return true;
		if (!busy && (status & slot_bit) == 0)
			return true;

		/* Waits before looking again. */
		kern_usleep_range(PACKET_WAIT_MIN_US, PACKET_WAIT_MAX_US);
	}

	/* The slot did not change in time. */
	return false;
}

/*
 * Stops the chosen port's audio: marks the FIFO's late data and errors,
 * takes the InfoFrame's slot out of service without waiting, and resets,
 * clears and flushes the FIFO.  The audio clock's gate stays open, since
 * the firmware may have opened it.  The port must take sound.  Control
 * mutex held.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/gpu/drm/vc4/vc4_hdmi.c (vc4_hdmi_audio_shutdown and
 * vc4_hdmi_audio_reset).
 */
static void
quiet_port(
	struct hdmi_audio *audio)
{
	struct hdmi_audio_port *port;
	unsigned long enabled;
	uint32_t fifo;
	uint32_t offset;
	uint32_t slot_bit;
	uint32_t config;

	port = &audio->ports[audio->port];
	fifo = port_fifo[audio->port];
	offset = drv_rpi5_hdmi_audio_packet_slot(DRV_RPI5_HDMI_AUDIO_INFOFRAME_TYPE);
	slot_bit = 1U << (offset / DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_BYTES);

	/* Writes the FIFO and the slot's bit under the lock. */
	enabled = spin_lock_irqsave(&audio->lock);

	write_register(&port->shared, fifo + FIFO_CONTROL,
		       DRV_RPI5_HDMI_AUDIO_FIFO_LATE | DRV_RPI5_HDMI_AUDIO_FIFO_UNDERFLOW | DRV_RPI5_HDMI_AUDIO_FIFO_OVERFLOW);
	config = read_register(&port->core, CORE_PACKET_CONFIG);
	write_register(&port->core, CORE_PACKET_CONFIG, config & ~slot_bit);
	write_register(&port->shared, fifo + FIFO_CONTROL, DRV_RPI5_HDMI_AUDIO_FIFO_RESET);
	write_register(&port->shared, fifo + FIFO_CONTROL, DRV_RPI5_HDMI_AUDIO_FIFO_OVERFLOW);
	write_register(&port->shared, fifo + FIFO_CONTROL, DRV_RPI5_HDMI_AUDIO_FIFO_FLUSH);

	spin_unlock_irqrestore(&audio->lock, enabled);
}

/*
 * Tells whether the port no longer holds what the driver wrote: the
 * firmware set the port up again, or changed the TMDS rate.  Notes the
 * FIFO's error bits on the way.  The port must take sound.  Control mutex
 * held.
 */
static bool
port_changed(
	struct hdmi_audio *audio)
{
	struct hdmi_audio_port *port;
	struct hdmi_audio_written *written;
	uint64_t tmds_hz;
	uint32_t fifo;
	uint32_t control;
	uint32_t gate;
	uint32_t config;
	uint32_t slot_bit;
	uint32_t sample;
	uint32_t format;
	uint32_t threshold;
	uint32_t regen;
	uint32_t cts;
	bool known;

	port = &audio->ports[audio->port];
	written = &audio->written;
	fifo = port_fifo[audio->port];
	slot_bit = 1U << (drv_rpi5_hdmi_audio_packet_slot(DRV_RPI5_HDMI_AUDIO_INFOFRAME_TYPE) / DRV_RPI5_HDMI_AUDIO_PACKET_SLOT_BYTES);

	/* The FIFO must still be enabled; its error bits are noted. */
	control = read_register(&port->shared, fifo + FIFO_CONTROL);
	audio->fifo_errors |= control & (DRV_RPI5_HDMI_AUDIO_FIFO_UNDERFLOW | DRV_RPI5_HDMI_AUDIO_FIFO_OVERFLOW);
	if ((control & DRV_RPI5_HDMI_AUDIO_FIFO_ENABLE) == 0)
		return true;

	/* The audio clock's gate must still be open. */
	gate = read_register(&audio->dvp, DVP_GATE);
	if ((gate & (1U << (DVP_GATE_FIRST_BIT + port->audio_clock))) != 0)
		return true;

	/* The FIFO's divider, format and thresholds must be the driver's. */
	sample = read_register(&port->shared, fifo + FIFO_SAMPLE_RATE);
	format = read_register(&port->shared, fifo + FIFO_FORMAT);
	threshold = read_register(&port->shared, fifo + FIFO_THRESHOLD);
	if (sample != written->sample || format != written->format)
		return true;
	if (threshold != written->threshold)
		return true;

	/* The clock regeneration must be the driver's; a CTS the port measures itself is not compared. */
	regen = read_register(&port->core, CORE_CLOCK_REGEN);
	cts = read_register(&port->core, CORE_CTS_0);
	if (regen != written->regen)
		return true;
	if (!HDMI_AUDIO_HARDWARE_CTS && cts != written->cts)
		return true;

	/* A sent InfoFrame must still be in service. */
	config = read_register(&port->core, CORE_PACKET_CONFIG);
	if (written->infoframe && (config & slot_bit) == 0)
		return true;

	/* The TMDS rate must still be known and must not have moved. */
	known = read_tmds(audio, &tmds_hz);
	if (!known)
		return true;
	if (tmds_hz != written->tmds_hz)
		return true;

	/* The port holds what the driver wrote. */
	return false;
}

/*
 * Starts the DMA channel at a fragment's control block.  Spin lock held.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (bcm2835_dma_start_desc for a 40-bit channel).
 */
static void
start_dma_locked(
	struct hdmi_audio *audio,
	uint32_t fragment)
{
	uint64_t block;

	/* The control block's address, shifted as the channel takes it. */
	block = audio->cb_bus + (uint64_t)fragment * sizeof(struct drv_rpi5_hdmi_audio_dma_cb);

	/* Points the channel at it and runs it. */
	write_register(&audio->channel_window, DMA_BLOCK, (uint32_t)(block >> DRV_RPI5_HDMI_AUDIO_DMA_CB_SHIFT));
	write_register(&audio->channel_window, DMA_STATUS, audio->run_word);
}

/*
 * Stops the DMA channel: lets its outstanding transactions finish, then
 * resets it and checks that it holds no control block.  Spins, briefly;
 * spin lock held.
 *
 * Procedure referenced: Raspberry Pi Linux rpi-6.12.y ab4372076bb8
 * drivers/dma/bcm2835-dma.c (bcm2835_dma_abort for a 40-bit channel).
 */
static void
halt_dma_locked(
	struct hdmi_audio *audio)
{
	uint32_t status;
	uint32_t block;
	uint32_t debug;
	unsigned polls;

	/* A channel without a control block has stopped. */
	block = read_register(&audio->channel_window, DMA_BLOCK);
	if (block == 0)
		return;

	/* Pauses it and waits for its outstanding transactions. */
	status = read_register(&audio->channel_window, DMA_STATUS);
	write_register(&audio->channel_window, DMA_STATUS, status & ~DRV_RPI5_HDMI_AUDIO_DMA_CS_RUN);
	for (polls = 0U; polls < DMA_STOP_POLLS; polls++) {
		status = read_register(&audio->channel_window, DMA_STATUS);
		if ((status & DMA_STATUS_TRANSACTIONS) == 0)
			break;
	}

	/* Resets it to its protected idle state. */
	write_register(&audio->channel_window, DMA_STATUS, DRV_RPI5_HDMI_AUDIO_DMA_CS_PROTECTION);
	debug = read_register(&audio->channel_window, DMA_DEBUG);
	write_register(&audio->channel_window, DMA_DEBUG, debug | DMA_DEBUG_RESET);

	/* Waits until the reset has cleared the control block. */
	for (polls = 0U; polls < DMA_STOP_POLLS; polls++) {
		block = read_register(&audio->channel_window, DMA_BLOCK);
		if (block == 0)
			break;
	}

	/* Reports a channel that still holds a block. */
	if (block != 0)
		kern_logf("rpi5-hdmi-audio: DMA channel %u did not stop (cb %08x)\n", audio->channel, block);
}

/*
 * Moves the stream's place to the fragment the DMA reads now and reports
 * how many fragments it finished since.  Spin lock held.
 */
static uint32_t
account_dma_locked(
	struct hdmi_audio *audio)
{
	uint32_t source;
	uint32_t source_info;
	uint32_t fragment;
	uint32_t pcm_offset;
	uint32_t finished;
	bool located;

	/* Where the DMA reads. */
	source = read_register(&audio->channel_window, DMA_SOURCE);
	source_info = read_register(&audio->channel_window, DMA_SOURCE_INFO);
	located = drv_rpi5_hdmi_audio_dma_locate(source, source_info, audio->data_bus,
						 DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES, DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT,
						 &fragment, &pcm_offset);
	if (!located)
		return 0;

	/* The fragments it went through, and its new place. */
	finished = drv_rpi5_hdmi_audio_steps(audio->current, fragment);
	audio->serial += finished;
	audio->current = fragment;
	atomic_raw_store_relaxed(&audio->position, fragment * DRV_RPI5_HDMI_AUDIO_PCM_FRAGMENT_BYTES);

	/* Succeeded: the finished fragments. */
	return finished;
}

/* Converts one fragment of the core's ring into its subframes, as the fragment with that serial. */
static void
convert_fragment(
	struct hdmi_audio *audio,
	uint32_t fragment,
	uint64_t serial)
{
	const int16_t *pcm;
	uint32_t *words;

	/* The fragment's samples and its subframes. */
	pcm = audio->pcm + (size_t)fragment * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * DRV_RPI5_HDMI_AUDIO_CHANNELS;
	words = audio->words + (size_t)fragment * DRV_RPI5_HDMI_AUDIO_FRAGMENT_FRAMES * DRV_RPI5_HDMI_AUDIO_CHANNELS;

	/* Converts them. */
	drv_rpi5_hdmi_audio_convert(pcm, words, serial, audio->status);
}

/*
 * Converts again the fragment the DMA reads and the next one, with what
 * the core copied meanwhile.  Spin lock held, stream running.
 */
static void
refresh_locked(
	struct hdmi_audio *audio)
{
	uint32_t next;

	/* The fragment after the current one, round the ring. */
	next = (audio->current + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT;

	/* Converts both. */
	convert_fragment(audio, audio->current, audio->serial);
	convert_fragment(audio, next, audio->serial + 1U);
}

/* Tells the audio core that fragments finished.  No driver lock held. */
static void
report_fragments(
	struct hdmi_audio *audio,
	uint32_t count)
{
	uint32_t index;

	/* One report per fragment. */
	for (index = 0U; index < count; index++)
		drv_audio_interrupt(audio->device, 0);
}

/*
 * Moves a running stream from the DMA to the clock: stops the DMA where it
 * is and lets the clock move the stream on from that fragment.  Control
 * mutex held.
 */
static void
enter_clock(
	struct hdmi_audio *audio,
	const char *reason)
{
	unsigned long enabled;
	uint32_t finished;

	/* Stops the DMA at its place and starts the clock there. */
	finished = 0;
	enabled = spin_lock_irqsave(&audio->lock);

	if (audio->mode == STREAM_DMA) {
		finished = account_dma_locked(audio);
		halt_dma_locked(audio);
	}
	atomic_raw_store_relaxed(&audio->mode, STREAM_CLOCK);
	atomic_raw_store_relaxed(&audio->position, audio->current * DRV_RPI5_HDMI_AUDIO_PCM_FRAGMENT_BYTES);
	audio->clock_base = sched_ticks();
	audio->clock_done = 0;

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Reports what the DMA finished before it stopped. */
	report_fragments(audio, finished);

	/* Says why, a few times a stream. */
	if (audio->changes_logged < WATCH_LOG_LIMIT) {
		audio->changes_logged++;
		kern_logf("rpi5-hdmi-audio: hdmi%u %s; the stream goes on without sound\n", audio->port, reason);
	}
}

/*
 * Moves a running stream from the clock to the DMA: sets the port up,
 * converts the fragment the stream is in and the next, and starts the DMA
 * there.  The port must take sound.  False when the port could not be set
 * up or the stream stopped meanwhile.  Control mutex held; may sleep.
 */
static bool
enter_dma(
	struct hdmi_audio *audio)
{
	unsigned long enabled;
	bool programmed;

	/* Sets the port up. */
	programmed = program_port(audio);
	if (!programmed)
		return false;

	/* Starts the DMA at the stream's place. */
	enabled = spin_lock_irqsave(&audio->lock);

	if (!audio->running) {
		spin_unlock_irqrestore(&audio->lock, enabled);
		return false;
	}
	refresh_locked(audio);
	atomic_raw_store_relaxed(&audio->mode, STREAM_DMA);
	start_dma_locked(audio, audio->current);

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Watches its progress from now. */
	audio->last_source = 0;
	audio->last_move_tick = sched_ticks();

	/* Succeeded: the DMA plays the stream. */
	return true;
}

/*
 * Moves a stream the clock runs on by the fragments that have played since
 * the clock started, and reports them.  Control mutex held.
 */
static void
run_clock(
	struct hdmi_audio *audio)
{
	unsigned long enabled;
	uint64_t elapsed_us;
	uint64_t due;
	uint32_t finished;

	/* The fragments due since the clock started. */
	finished = 0;
	enabled = spin_lock_irqsave(&audio->lock);

	if (!audio->running || audio->mode != STREAM_CLOCK) {
		spin_unlock_irqrestore(&audio->lock, enabled);
		return;
	}
	elapsed_us = kern_ticks_to_us(sched_ticks() - audio->clock_base);
	due = drv_rpi5_hdmi_audio_clock_due(elapsed_us);

	/* A clock far behind drops all but the last ring's worth. */
	if (due > audio->clock_done + DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT)
		audio->clock_done = due - DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT;

	/* Moves the stream on one fragment at a time. */
	while (audio->clock_done < due) {
		audio->current = (audio->current + 1U) % DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT;
		audio->serial++;
		audio->clock_done++;
		finished++;
	}
	atomic_raw_store_relaxed(&audio->position, audio->current * DRV_RPI5_HDMI_AUDIO_PCM_FRAGMENT_BYTES);

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Reports them. */
	report_fragments(audio, finished);
}

/*
 * Tells whether the DMA still moves: its source address changed since the
 * last look, or has stood still for less than two fragments.  Control
 * mutex held.
 */
static bool
dma_moving(
	struct hdmi_audio *audio,
	uint64_t now)
{
	uint64_t limit;
	uint32_t source;

	/* A changed address is progress. */
	source = read_register(&audio->channel_window, DMA_SOURCE);
	if (source != audio->last_source) {
		audio->last_source = source;
		audio->last_move_tick = now;
		return true;
	}

	/* An address that stood still too long is a stall. */
	limit = kern_ms_to_ticks(WATCH_STALL_MS);
	if (now - audio->last_move_tick > limit)
		return false;

	/* Not long enough to tell. */
	return true;
}

/*
 * Looks at a running stream once: keeps the DMA on a port that takes
 * sound and holds the driver's set-up, and otherwise moves the stream on
 * by the clock, going back to the DMA once the port takes sound again.
 * Control mutex held; may sleep.
 */
static void
watch_once(
	struct hdmi_audio *audio)
{
	unsigned long enabled;
	unsigned mode;
	uint64_t now;
	uint64_t window;
	bool running;
	bool alive;
	bool changed;
	bool moving;
	bool entered;

	/* What moves the stream now. */
	enabled = spin_lock_irqsave(&audio->lock);

	running = audio->running;
	mode = audio->mode;

	spin_unlock_irqrestore(&audio->lock, enabled);
	if (!running)
		return;
	now = sched_ticks();

	/* A stream on the clock goes on, and back to the DMA when it can. */
	if (mode == STREAM_CLOCK) {
		run_clock(audio);
		if (now < audio->retry_tick)
			return;
		alive = port_alive(audio, false);
		if (!alive)
			return;
		entered = enter_dma(audio);
		if (!entered)
			audio->retry_tick = now + kern_ms_to_ticks(WATCH_RETRY_MS);
		return;
	}

	/* A port that no longer takes sound hands the stream to the clock. */
	alive = port_alive(audio, false);
	if (!alive) {
		enter_clock(audio, "stopped taking sound");
		audio->retry_tick = now;
		return;
	}

	/* A port the firmware set up again is set up for the stream again. */
	changed = false;
	if (audio->compare)
		changed = port_changed(audio);
	if (changed) {
		/* Counts set-ups that follow each other closely; a lone one starts a new run. */
		window = kern_ms_to_ticks(WATCH_REWRITE_MS);
		if (now - audio->rewrite_tick < window) {
			audio->rewrites++;
		} else {
			audio->rewrites = 1U;
		}
		audio->rewrite_tick = now;

		/* A port that never reads back what was written is no longer compared. */
		if (audio->rewrites > WATCH_REWRITE_LIMIT) {
			audio->compare = false;
			kern_logf("rpi5-hdmi-audio: hdmi%u does not read back its set-up; no longer compared\n", audio->port);
			return;
		}

		/* Sets the port up again and goes on from the same place. */
		enter_clock(audio, "was changed by the firmware");
		entered = enter_dma(audio);
		if (!entered)
			audio->retry_tick = now + kern_ms_to_ticks(WATCH_RETRY_MS);
		return;
	}

	/* A DMA that stopped moving hands the stream to the clock for a while. */
	moving = dma_moving(audio, now);
	if (!moving) {
		enter_clock(audio, "stopped reading sound");
		audio->retry_tick = now + kern_ms_to_ticks(WATCH_RETRY_MS);
	}
}

/*
 * Watches the running stream every WATCH_PERIOD_MS, and sleeps while no
 * stream runs, for as long as the kernel runs.
 */
static void
watch_thread(
	void *argument)
{
	struct hdmi_audio *audio;
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;

	audio = argument;

	/* For as long as the kernel runs. */
	for (;;) {
		/* Sleeps until a stream runs. */
		enabled = spin_lock_irqsave(&audio->lock);

		while (!audio->running) {
			observed = waitq_sequence(&audio->wake);
			(void)waitq_sleep(&audio->wake, &audio->lock, observed, 0, 0);
		}

		spin_unlock_irqrestore(&audio->lock, enabled);

		/* Waits for the period. */
		deadline = sched_ticks() + kern_ms_to_ticks(WATCH_PERIOD_MS);
		sched_sleep(deadline);

		/* Looks at the stream, in turn with prepare, start and stop. */
		mutex_lock(&audio->control);

		watch_once(audio);

		mutex_unlock(&audio->control);
	}
}

/*
 * Prepares a playback stream: takes the core's ring and converts all of
 * it, each fragment as the one the stream plays at its turn.
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
	struct hdmi_audio *audio;
	uint32_t fragment;

	audio = private_data;

	/* Playback only, in the one format. */
	if (capture != 0)
		return ENOTSUP;
	if (format->format != KERN_AUDIO_FORMAT_S16_LE || format->channels != DRV_RPI5_HDMI_AUDIO_CHANNELS)
		return EINVAL;
	if (format->rate != DRV_RPI5_HDMI_AUDIO_RATE)
		return EINVAL;

	/* The ring the subframe ring mirrors, fragment for fragment. */
	if (fragment_bytes != DRV_RPI5_HDMI_AUDIO_PCM_FRAGMENT_BYTES || fragment_count != DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT)
		return EINVAL;
	if (buffer->size < (size_t)fragment_bytes * fragment_count)
		return EINVAL;

	/* Converts the whole ring while no stream runs, so no interrupt reads it. */
	mutex_lock(&audio->control);

	audio->pcm = buffer->address;
	for (fragment = 0U; fragment < DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT; fragment++)
		convert_fragment(audio, fragment, fragment);
	audio->prepared = true;

	mutex_unlock(&audio->control);

	/* Succeeded: the stream can start. */
	return 0;
}

/*
 * Starts the stream at the ring's first fragment: on the DMA when the port
 * takes sound, otherwise on the clock, and wakes the watching thread.
 */
static int
audio_start(
	void *private_data,
	int capture)
{
	struct hdmi_audio *audio;
	unsigned long enabled;
	uint64_t now;
	bool alive;
	bool entered;

	audio = private_data;

	/* Playback only. */
	if (capture != 0)
		return ENOTSUP;

	/* Refuses a start without a prepare. */
	mutex_lock(&audio->control);

	if (!audio->prepared) {
		mutex_unlock(&audio->control);
		return EINVAL;
	}

	/* Brings the first fragments up to date, then runs the stream on the clock from the start. */
	convert_fragment(audio, 0U, 0U);
	convert_fragment(audio, 1U, 1U);
	now = sched_ticks();
	enabled = spin_lock_irqsave(&audio->lock);

	audio->current = 0U;
	audio->serial = 0U;
	audio->clock_base = now;
	audio->clock_done = 0U;
	audio->dma_errors = 0U;
	atomic_raw_store_relaxed(&audio->position, 0U);
	atomic_raw_store_relaxed(&audio->mode, STREAM_CLOCK);
	audio->running = true;

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Hands it to the DMA when the port takes sound. */
	audio->fifo_errors = 0U;
	audio->changes_logged = 0U;
	audio->compare = true;
	audio->rewrites = 0U;
	audio->rewrite_tick = 0U;
	audio->retry_tick = now;
	entered = false;
	alive = port_alive(audio, true);
	if (alive) {
		/* A port that takes sound but could not be set up is tried again later. */
		entered = enter_dma(audio);
		if (!entered)
			audio->retry_tick = now + kern_ms_to_ticks(WATCH_RETRY_MS);
	}

	/* Says that the stream starts without sound. */
	if (!entered)
		kern_logf("rpi5-hdmi-audio: hdmi%u takes no sound now; the stream goes on without sound\n", audio->port);

	/* Wakes the watching thread. */
	enabled = spin_lock_irqsave(&audio->lock);

	waitq_wake_all(&audio->wake);

	spin_unlock_irqrestore(&audio->lock, enabled);
	mutex_unlock(&audio->control);

	/* Succeeded: the stream runs. */
	return 0;
}

/*
 * Stops the stream: marks it stopped, stops the DMA, and quiets the port
 * when it still takes sound.  Once this returns neither the interrupt nor
 * the thread touches the core's ring.
 */
static void
audio_stop(
	void *private_data,
	int capture)
{
	struct hdmi_audio *audio;
	unsigned long enabled;
	unsigned errors;
	bool alive;

	audio = private_data;

	/* Playback only. */
	if (capture != 0)
		return;

	/* Marks the stream stopped and stops the DMA. */
	mutex_lock(&audio->control);
	enabled = spin_lock_irqsave(&audio->lock);

	audio->running = false;
	if (audio->mode == STREAM_DMA)
		halt_dma_locked(audio);
	atomic_raw_store_relaxed(&audio->mode, STREAM_IDLE);
	errors = audio->dma_errors;

	spin_unlock_irqrestore(&audio->lock, enabled);

	/* Quiets a port the stream set up, if it still answers. */
	if (audio->programmed) {
		alive = port_alive(audio, true);
		if (alive)
			quiet_port(audio);
		audio->programmed = false;
	}

	/* Says what went wrong during the stream. */
	if (errors != 0 || audio->fifo_errors != 0)
		kern_logf("rpi5-hdmi-audio: hdmi%u stream had %u DMA errors, FIFO error bits %08x\n",
			  audio->port, errors, audio->fifo_errors);

	mutex_unlock(&audio->control);
}

/*
 * Tells where the stream is in the core's ring: where the DMA reads, or
 * the start of the fragment the clock has reached.  Takes no lock.
 */
static uint32_t
audio_position(
	void *private_data,
	int capture)
{
	struct hdmi_audio *audio;
	unsigned mode;
	uint32_t source;
	uint32_t source_info;
	uint32_t fragment;
	uint32_t pcm_offset;
	bool located;

	audio = private_data;

	/* Playback only. */
	if (capture != 0)
		return 0;

	/* The DMA's source address, while the DMA plays. */
	mode = atomic_raw_load_relaxed(&audio->mode);
	if (mode == STREAM_DMA) {
		source = read_register(&audio->channel_window, DMA_SOURCE);
		source_info = read_register(&audio->channel_window, DMA_SOURCE_INFO);
		located = drv_rpi5_hdmi_audio_dma_locate(source, source_info, audio->data_bus,
							 DRV_RPI5_HDMI_AUDIO_WIRE_FRAGMENT_BYTES, DRV_RPI5_HDMI_AUDIO_FRAGMENT_COUNT,
							 &fragment, &pcm_offset);
		if (located)
			return pcm_offset;
	}

	/* Otherwise the place the stream last reached. */
	return atomic_raw_load_relaxed(&audio->position);
}

/*
 * Handles a fragment's end: clears the channel's interrupt, moves the
 * stream's place, reports the finished fragments, and converts the
 * fragment the DMA reads and the next with what the core copied.  Writes
 * no port register.
 */
static void
audio_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct hdmi_audio *audio;
	unsigned long enabled;
	uint32_t status;
	uint32_t debug;
	uint32_t finished;
	bool first_error;

	(void)irq;
	audio = argument;
	first_error = false;

	/* Clears the interrupt of a channel the stream no longer runs, leaving it stopped. */
	enabled = spin_lock_irqsave(&audio->lock);

	status = read_register(&audio->channel_window, DMA_STATUS);
	if (!audio->running || audio->mode != STREAM_DMA) {
		write_register(&audio->channel_window, DMA_STATUS,
			       DRV_RPI5_HDMI_AUDIO_DMA_CS_INTERRUPT | DRV_RPI5_HDMI_AUDIO_DMA_CS_PROTECTION);
		spin_unlock_irqrestore(&audio->lock, enabled);
		kern_irq_send_eoi(acknowledge);
		return;
	}

	/* Clears it on a running channel, which keeps running. */
	write_register(&audio->channel_window, DMA_STATUS, audio->run_word | DRV_RPI5_HDMI_AUDIO_DMA_CS_INTERRUPT);

	/* Counts a DMA error. */
	if ((status & DMA_STATUS_ERROR) != 0) {
		if (audio->dma_errors == 0)
			first_error = true;
		audio->dma_errors++;
	}

	/* Moves the stream's place to where the DMA reads. */
	finished = account_dma_locked(audio);

	spin_unlock_irqrestore(&audio->lock, enabled);
	kern_irq_send_eoi(acknowledge);

	/* Says the first error of a stream. */
	if (first_error) {
		debug = read_register(&audio->channel_window, DMA_DEBUG);
		kern_logf("rpi5-hdmi-audio: DMA error, cs %08x debug %08x\n", status, debug);
	}

	/* Reports the finished fragments outside the driver's lock. */
	report_fragments(audio, finished);

	/* Converts the fragment the DMA reads and the next, after the core's copy. */
	enabled = spin_lock_irqsave(&audio->lock);

	if (audio->running && audio->mode == STREAM_DMA)
		refresh_locked(audio);

	spin_unlock_irqrestore(&audio->lock, enabled);
}

/* Reads one 32-bit register of a mapped window. */
static uint32_t
read_register(
	const struct hdmi_audio_window *window,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register. */
	value = kern_mmio_read32(window->mapped + offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one 32-bit register of a mapped window. */
static void
write_register(
	const struct hdmi_audio_window *window,
	uint32_t offset,
	uint32_t value)
{
	/* Writes the register. */
	kern_mmio_write32(window->mapped + offset, value);
}
